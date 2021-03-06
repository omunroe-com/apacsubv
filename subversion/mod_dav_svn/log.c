/*
 * log.c: handle the log-report request and response
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_xml.h>
#include <mod_dav.h>

#include "svn_pools.h"
#include "svn_repos.h"
#include "svn_fs.h"
#include "svn_xml.h"
#include "svn_path.h"

#include "dav_svn.h"


struct log_receiver_baton
{
  /* this buffers the output for a bit and is automatically flushed,
     at appropriate times, by the Apache filter system. */
  apr_bucket_brigade *bb;

  /* where to deliver the output */
  ap_filter_t *output;

  /* For temporary allocations. */
  apr_pool_t *pool;
};


static void send_xml(struct log_receiver_baton *lrb, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  (void) apr_brigade_vprintf(lrb->bb, ap_filter_flush, lrb->output, fmt, ap);
  va_end(ap);
}


/* This implements `svn_log_message_receiver_t'. */
static svn_error_t * log_receiver(void *baton,
                                  apr_hash_t *changed_paths,
                                  svn_revnum_t rev,
                                  const char *author,
                                  const char *date,
                                  const char *msg)
{
  struct log_receiver_baton *lrb = baton;

  send_xml(lrb,
           "<S:log-item>" DEBUG_CR
           "<D:version-name>%lu</D:version-name>" DEBUG_CR
           "<D:creator-displayname>%s</D:creator-displayname>" DEBUG_CR
           /* ### this should be DAV:creation-date, but we need to format
              ### that date a bit differently */
           "<S:date>%s</S:date>" DEBUG_CR
           "<D:comment>%s</D:comment>" DEBUG_CR,
           rev,
           apr_xml_quote_string(lrb->pool, author, 0),
           apr_xml_quote_string(lrb->pool, date, 0),
           apr_xml_quote_string(lrb->pool, msg, 0));

  if (changed_paths)
    {
      apr_hash_index_t *hi;
      char *path;

      for (hi = apr_hash_first(lrb->pool, changed_paths);
           hi != NULL;
           hi = apr_hash_next(hi))
        {
          void *val;
          char action;
          
          apr_hash_this(hi, (void *) &path, NULL, &val);
          action = (char) ((int) val);

          /* ### todo: is there a D: namespace equivalent for
             `changed-path'?  Should use it if so. */
          if (action == 'A')
            send_xml(lrb, "<S:added-path>%s</S:added-path>" DEBUG_CR,
                     apr_xml_quote_string(lrb->pool, path, 0));
          else if (action == 'D')
            send_xml(lrb, "<S:deleted-path>%s</S:deleted-path>" DEBUG_CR,
                     apr_xml_quote_string(lrb->pool, path, 0));
          else
            send_xml(lrb, "<S:changed-path>%s</S:changed-path>" DEBUG_CR,
                     apr_xml_quote_string(lrb->pool, path, 0));
        }
    }

  send_xml(lrb, "</S:log-item>" DEBUG_CR);

  /* Clear out anything we may have placed in here. */
  svn_pool_clear(lrb->pool);

  return SVN_NO_ERROR;
}




dav_error * dav_svn__log_report(const dav_resource *resource,
                                const apr_xml_doc *doc,
                                ap_filter_t *output)
{
  svn_error_t *serr;
  apr_xml_elem *child;
  struct log_receiver_baton lrb;
  const dav_svn_repos *repos = resource->info->repos;
  svn_stringbuf_t *target = NULL; 
  int ns;

  /* These get determined from the request document. */
  svn_revnum_t start = SVN_INVALID_REVNUM;   /* defaults to HEAD */
  svn_revnum_t end = SVN_INVALID_REVNUM;     /* defaults to HEAD */
  svn_boolean_t discover_changed_paths = 0;  /* off by default */

  /* ### why are these paths stringbuf? they aren't going to be changed... */
  apr_array_header_t *paths
    = apr_array_make(resource->pool, 0, sizeof(svn_stringbuf_t *));

  /* Sanity check. */
  ns = dav_svn_find_ns(doc->namespaces, SVN_XML_NAMESPACE);
  if (ns == -1)
    {
      return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
                           "The request does not contain the 'svn:' "
                           "namespace, so it is not going to have certain "
                           "required elements.");
    }
  
  /* ### todo: okay, now go fill in svn_ra_dav__get_log() based on the
     syntax implied below... */
  for (child = doc->root->first_child; child != NULL; child = child->next)
    {
      /* if this element isn't one of ours, then skip it */
      if (child->ns != ns)
        continue;

      if (strcmp(child->name, "start-revision") == 0)
        {
          /* ### assume no white space, no child elems, etc */
          start = SVN_STR_TO_REV(child->first_cdata.first->text);
        }
      else if (strcmp(child->name, "end-revision") == 0)
        {
          /* ### assume no white space, no child elems, etc */
          end = SVN_STR_TO_REV(child->first_cdata.first->text);
        }
      else if (strcmp(child->name, "discover-changed-paths") == 0)
        {
          /* ### todo: value doesn't matter, presence alone is enough?
             (I.e., is that a traditional way to do things here?) */
          discover_changed_paths = 1;
        }
      else if (strcmp(child->name, "path") == 0)
        {
          /* Convert these relative paths to absolute paths in the
             repository. */

          target = svn_stringbuf_create (resource->info->repos_path,
                                         resource->pool);
          /* Don't add on an empty string, but do add the target to the
             path.  This special case means that we have passed a single
             directory to get the log of, and we need a path to call
             svn_fs_revisions_changed on. */
          if (child->first_cdata.first)
            svn_path_add_component_nts (target,
                                        child->first_cdata.first->text);

          (*((svn_stringbuf_t **)(apr_array_push (paths)))) = target;
        }
      /* else unknown element; skip it */
    }

  lrb.bb = apr_brigade_create(resource->pool,  /* not the subpool! */
                              output->c->bucket_alloc);
  lrb.output = output;
  lrb.pool = svn_pool_create(resource->pool);

  /* Start the log report. */
  send_xml(&lrb,
           DAV_XML_HEADER DEBUG_CR
           "<S:log-report xmlns:S=\"" SVN_XML_NAMESPACE "\" "
           "xmlns:D=\"DAV:\">" DEBUG_CR);

  /* Send zero or more log items. */
  serr = svn_repos_get_logs(repos->repos,
                            paths,
                            start,
                            end,
                            discover_changed_paths,
                            log_receiver,
                            &lrb,
                            resource->pool);

  /* End the log report. */
  send_xml(&lrb, "</S:log-report>" DEBUG_CR);

  /* flush the contents of the brigade */
  ap_fflush(output, lrb.bb);

  /* All done with the pool! */
  svn_pool_destroy(lrb.pool);

  if (serr)
    {
      /* NOTE: we've definitely generated some content into the output
         filter, which means that we cannot return an error here.
         Oh well... */
      /* ### in the future, mod_dav may specify a way to signal an error
         ### even after the response stream has begun */
      return NULL;
    }
  
  return NULL;
}



/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */
