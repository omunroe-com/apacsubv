/*
 * diff_editor.c -- The diff editor for comparing the working copy against the
 *                  repository.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/*
 * This code uses an svn_delta_editor_t editor driven by
 * svn_wc_crawl_revisions (like the update command) to retrieve the
 * differences between the working copy and the requested repository
 * version. Rather than updating the working copy, this new editor creates
 * temporary files that contain the pristine repository versions. When the
 * crawler closes the files the editor calls back to a client layer
 * function to compare the working copy and the temporary file. There is
 * only ever one temporary file in existence at any time.
 *
 * When the crawler closes a directory, the editor then calls back to the
 * client layer to compare any remaining files that may have been modified
 * locally. Added directories do not have corresponding temporary
 * directories created, as they are not needed.
 *
 * The diff result from this editor is a combination of the restructuring
 * operations from the repository with the local restructurings since checking
 * out.
 *
 * ### TODO: Make sure that we properly support and report multi layered
 *           operations instead of only simple file replacements.
 *
 * ### TODO: Replacements where the node kind changes needs support. It
 * mostly works when the change is in the repository, but not when it is
 * in the working copy.
 *
 * ### TODO: Do we need to support copyfrom?
 *
 */

#include <apr_hash.h>
#include <apr_md5.h>

#include <assert.h>

#include "svn_error.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"

#include "private/svn_subr_private.h"
#include "private/svn_wc_private.h"
#include "private/svn_diff_tree.h"
#include "private/svn_editor.h"

#include "wc.h"
#include "props.h"
#include "adm_files.h"
#include "translate.h"

#include "svn_private_config.h"


/*-------------------------------------------------------------------------*/
/* A little helper function.

   You see, when we ask the server to update us to a certain revision,
   we construct the new fulltext, and then run

         'diff <repos_fulltext> <working_fulltext>'

   which is, of course, actually backwards from the repository's point
   of view.  It thinks we want to move from working->repos.

   So when the server sends property changes, they're effectively
   backwards from what we want.  We don't want working->repos, but
   repos->working.  So this little helper "reverses" the value in
   BASEPROPS and PROPCHANGES before we pass them off to the
   prop_changed() diff-callback.  */
static void
reverse_propchanges(apr_hash_t *baseprops,
                    apr_array_header_t *propchanges,
                    apr_pool_t *pool)
{
  int i;

  /* ### todo: research lifetimes for property values below */

  for (i = 0; i < propchanges->nelts; i++)
    {
      svn_prop_t *propchange
        = &APR_ARRAY_IDX(propchanges, i, svn_prop_t);

      const svn_string_t *original_value =
        apr_hash_get(baseprops, propchange->name, APR_HASH_KEY_STRING);

      if ((original_value == NULL) && (propchange->value != NULL))
        {
          /* found an addition.  make it look like a deletion. */
          apr_hash_set(baseprops, propchange->name, APR_HASH_KEY_STRING,
                       svn_string_dup(propchange->value, pool));
          propchange->value = NULL;
        }

      else if ((original_value != NULL) && (propchange->value == NULL))
        {
          /* found a deletion.  make it look like an addition. */
          propchange->value = svn_string_dup(original_value, pool);
          apr_hash_set(baseprops, propchange->name, APR_HASH_KEY_STRING,
                       NULL);
        }

      else if ((original_value != NULL) && (propchange->value != NULL))
        {
          /* found a change.  just swap the values.  */
          const svn_string_t *str = svn_string_dup(propchange->value, pool);
          propchange->value = svn_string_dup(original_value, pool);
          apr_hash_set(baseprops, propchange->name, APR_HASH_KEY_STRING, str);
        }
    }
}


/* Set *RESULT_ABSPATH to the absolute path to a readable file containing
   the pristine text of LOCAL_ABSPATH in DB, or to NULL if it does not have
   any pristine text.

   If USE_BASE is FALSE it gets the pristine text of what is currently in the
   working copy. (So it returns the pristine file of a copy).

   If USE_BASE is TRUE, it looks in the lowest layer of the working copy and
   shows exactly what was originally checked out (or updated to).

   Rationale:

   Which text-base do we want to use for the diff?  If the node is replaced
   by a new file, then the base of the replaced file is called (in WC-1) the
   "revert base".  If the replacement is a copy or move, then there is also
   the base of the copied file to consider.

   One could argue that we should never diff against the revert
   base, and instead diff against the empty-file for both types of
   replacement.  After all, there is no ancestry relationship
   between the working file and the base file.  But my guess is that
   in practice, users want to see the diff between their working
   file and "the nearest versioned thing", whatever that is.  I'm
   not 100% sure this is the right decision, but it at least seems
   to match our test suite's expectations. */
static svn_error_t *
get_pristine_file(const char **result_abspath,
                  svn_wc__db_t *db,
                  const char *local_abspath,
                  svn_boolean_t use_base,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  const svn_checksum_t *checksum;

  if (!use_base)
    {
      SVN_ERR(svn_wc__db_read_pristine_info(NULL, NULL, NULL, NULL, NULL, NULL,
                                            &checksum, NULL, NULL, NULL,
                                            db, local_abspath,
                                            scratch_pool, scratch_pool));
    }
  else
    {
      SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, &checksum,
                                       NULL, NULL, NULL, NULL, NULL,
                                       db, local_abspath,
                                       scratch_pool, scratch_pool));
    }

  if (checksum != NULL)
    {
      SVN_ERR(svn_wc__db_pristine_get_path(result_abspath, db, local_abspath,
                                           checksum,
                                           result_pool, scratch_pool));
      return SVN_NO_ERROR;
    }

  *result_abspath = NULL;
  return SVN_NO_ERROR;
}


/*-------------------------------------------------------------------------*/


/* Overall crawler editor baton.
 */
struct edit_baton_t 
{
  /* A wc db. */
  svn_wc__db_t *db;

  /* A diff tree processor, receiving the result of the diff. */
  const svn_diff_tree_processor_t *processor;

  /* A boolean indicating whether local additions should be reported before
     remote deletes. The processor can transform adds in deletes and deletes
     in adds, but it can't reorder the output. */
  svn_boolean_t local_before_remote;

  /* ANCHOR/TARGET represent the base of the hierarchy to be compared. */
  const char *target;
  const char *anchor_abspath;

  /* Target revision */
  svn_revnum_t revnum;

  /* Was the root opened? */
  svn_boolean_t root_opened;

  /* How does this diff descend as seen from target? */
  svn_depth_t depth;

  /* Should this diff ignore node ancestry? */
  svn_boolean_t ignore_ancestry;

  /* Should this diff not compare copied files with their source? */
  svn_boolean_t show_copies_as_adds;

  /* Possibly diff repos against text-bases instead of working files. */
  svn_boolean_t diff_pristine;

  /* Hash whose keys are const char * changelist names. */
  apr_hash_t *changelist_hash;

  /* Cancel function/baton */
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  apr_pool_t *pool;
};

/* Directory level baton.
 */
struct dir_baton_t
{
  /* Gets set if the directory is added rather than replaced/unchanged. */
  svn_boolean_t added;

  /* Reference to parent directory baton (or NULL for the root) */
  struct dir_baton_t *parent_baton;

  /* The depth at which this directory should be diffed. */
  svn_depth_t depth;

  /* The name and path of this directory as if they would be/are in the
      local working copy. */
  const char *name;
  const char *local_abspath;

  /* The "correct" path of the directory, but it may not exist in the
     working copy. */
  const char *path;

  /* Processor state */
  void *pdb;
  svn_boolean_t skip;
  svn_boolean_t skip_children;

  svn_diff_source_t *left_src;
  svn_diff_source_t *right_src;

  /* Identifies those directory elements that get compared while running
     the crawler.  These elements should not be compared again when
     recursively looking for local modifications.

     This hash maps the full path of the entry to an unimportant value
     (presence in the hash is the important factor here, not the value
     itself).

     If the directory's properties have been compared, an item with hash
     key of path will be present in the hash. */
  apr_hash_t *compared;

  /* The list of incoming BASE->repos propchanges. */
  apr_array_header_t *propchanges;

  /* Has a change on regular properties */
  svn_boolean_t has_propchange;

  /* The overall crawler editor baton. */
  struct edit_baton_t *eb;

  apr_pool_t *pool;
  int users;
};

/* File level baton.
 */
struct file_baton_t
{
  /* Gets set if the file is added rather than replaced. */
  svn_boolean_t added;

  struct dir_baton_t *parent_baton;

  /* The name and path of this file as if they would be/are in the
      local working copy. */
  const char *name;
  const char *local_abspath;

  /* Processor state */
  void *pfb;
  svn_boolean_t skip;

  const svn_diff_source_t *left_src;
  const svn_diff_source_t *right_src;

  /* PATH is the "correct" path of the file, but it may not exist in the
     working copy */
  const char *path;

 /* When constructing the requested repository version of the file, we
    drop the result into a file at TEMP_FILE_PATH. */
  const char *temp_file_path;

  /* The list of incoming BASE->repos propchanges. */
  apr_array_header_t *propchanges;

  /* Has a change on regular properties */
  svn_boolean_t has_propchange;

  /* The current checksum on disk */
  const svn_checksum_t *base_checksum;

  /* The resulting checksum from apply_textdelta */
  svn_checksum_t *result_checksum;

  /* The overall crawler editor baton. */
  struct edit_baton_t *eb;

  apr_pool_t *pool;
};

/* Create a new edit baton. TARGET_PATH/ANCHOR are working copy paths
 * that describe the root of the comparison. CALLBACKS/CALLBACK_BATON
 * define the callbacks to compare files. DEPTH defines if and how to
 * descend into subdirectories; see public doc string for exactly how.
 * IGNORE_ANCESTRY defines whether to utilize node ancestry when
 * calculating diffs.  USE_TEXT_BASE defines whether to compare
 * against working files or text-bases.  REVERSE_ORDER defines which
 * direction to perform the diff.
 *
 * CHANGELIST_FILTER is a list of const char * changelist names, used to
 * filter diff output responses to only those items in one of the
 * specified changelists, empty (or NULL altogether) if no changelist
 * filtering is requested.
 */
static svn_error_t *
make_edit_baton(struct edit_baton_t **edit_baton,
                svn_wc__db_t *db,
                const char *anchor_abspath,
                const char *target,
                const svn_wc_diff_callbacks4_t *callbacks,
                void *callback_baton,
                svn_depth_t depth,
                svn_boolean_t ignore_ancestry,
                svn_boolean_t show_copies_as_adds,
                svn_boolean_t use_git_diff_format,
                svn_boolean_t use_text_base,
                svn_boolean_t reverse_order,
                const apr_array_header_t *changelist_filter,
                svn_cancel_func_t cancel_func,
                void *cancel_baton,
                apr_pool_t *pool)
{
  apr_hash_t *changelist_hash = NULL;
  struct edit_baton_t *eb;
  const svn_diff_tree_processor_t *processor;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(anchor_abspath));

  if (changelist_filter && changelist_filter->nelts)
    SVN_ERR(svn_hash_from_cstring_keys(&changelist_hash, changelist_filter,
                                       pool));

  SVN_ERR(svn_wc__wrap_diff_callbacks(&processor,
                                      callbacks, callback_baton, TRUE,
                                      pool, pool));

  if (reverse_order)
    processor = svn_diff__tree_processor_reverse_create(processor, NULL, pool);

  if (! show_copies_as_adds && !use_git_diff_format)
    processor = svn_diff__tree_processor_copy_as_changed_create(processor,
                                                                pool);

  eb = apr_pcalloc(pool, sizeof(*eb));
  eb->db = db;
  eb->anchor_abspath = apr_pstrdup(pool, anchor_abspath);
  eb->target = apr_pstrdup(pool, target);
  eb->processor = processor;
  eb->depth = depth;
  eb->ignore_ancestry = ignore_ancestry;
  eb->show_copies_as_adds = show_copies_as_adds;
  eb->local_before_remote = reverse_order;
  eb->diff_pristine = use_text_base;
  eb->changelist_hash = changelist_hash;
  eb->cancel_func = cancel_func;
  eb->cancel_baton = cancel_baton;
  eb->pool = pool;

  *edit_baton = eb;
  return SVN_NO_ERROR;
}

/* Create a new directory baton.  PATH is the directory path,
 * including anchor_path.  ADDED is set if this directory is being
 * added rather than replaced.  PARENT_BATON is the baton of the
 * parent directory, it will be null if this is the root of the
 * comparison hierarchy.  The directory and its parent may or may not
 * exist in the working copy.  EDIT_BATON is the overall crawler
 * editor baton.
 */
static struct dir_baton_t *
make_dir_baton(const char *path,
               struct dir_baton_t *parent_baton,
               struct edit_baton_t *eb,
               svn_boolean_t added,
               svn_depth_t depth,
               apr_pool_t *result_pool)
{
  apr_pool_t *dir_pool = svn_pool_create(parent_baton ? parent_baton->pool
                                                      : eb->pool);
  struct dir_baton_t *db = apr_pcalloc(dir_pool, sizeof(*db));

  db->parent_baton = parent_baton;
  db->eb = eb;
  db->added = added;
  db->depth = depth;
  db->pool = dir_pool;
  db->propchanges = apr_array_make(dir_pool, 8, sizeof(svn_prop_t));
  db->compared = apr_hash_make(dir_pool);
  db->path = apr_pstrdup(dir_pool, path);

  db->name = svn_dirent_basename(db->path, NULL);

  if (parent_baton != NULL)
    {
      db->local_abspath = svn_dirent_join(parent_baton->local_abspath,
                                          db->name,
                                          dir_pool);
      parent_baton->users++;
    }
  else
    db->local_abspath = apr_pstrdup(dir_pool, eb->anchor_abspath);

  db->users = 1;

  return db;
}

/* Create a new file baton.  PATH is the file path, including
 * anchor_path.  ADDED is set if this file is being added rather than
 * replaced.  PARENT_BATON is the baton of the parent directory.
 * The directory and its parent may or may not exist in the working copy.
 */
static struct file_baton_t *
make_file_baton(const char *path,
                svn_boolean_t added,
                struct dir_baton_t *parent_baton,
                apr_pool_t *result_pool)
{
  apr_pool_t *file_pool = svn_pool_create(result_pool);
  struct file_baton_t *fb = apr_pcalloc(file_pool, sizeof(*fb));
  struct edit_baton_t *eb = parent_baton->eb;

  fb->eb = eb;
  fb->parent_baton = parent_baton;
  fb->parent_baton->users++;
  fb->added = added;
  fb->pool = file_pool;
  fb->propchanges  = apr_array_make(file_pool, 8, sizeof(svn_prop_t));
  fb->path = apr_pstrdup(file_pool, path);

  fb->name = svn_dirent_basename(fb->path, NULL);
  fb->local_abspath = svn_dirent_join(parent_baton->local_abspath, fb->name,
                                      file_pool);

  return fb;
}

/* Destroy DB when there are no more registered users */
static svn_error_t *
maybe_done(struct dir_baton_t *db)
{
  db->users--;

  if (!db->users)
    {
      struct dir_baton_t *pb = db->parent_baton;

      svn_pool_clear(db->pool);

      if (pb != NULL)
        SVN_ERR(maybe_done(pb));
    }

  return SVN_NO_ERROR;
}

/* Diff the file PATH against its text base.  At this
 * stage we are dealing with a file that does exist in the working copy.
 *
 * DIR_BATON is the parent directory baton, PATH is the path to the file to
 * be compared.
 *
 * Do all allocation in POOL.
 *
 * ### TODO: Need to work on replace if the new filename used to be a
 * directory.
 */
static svn_error_t *
file_diff(struct edit_baton_t *eb,
          const char *local_abspath,
          const char *path,
          void *dir_baton,
          apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = eb->db;
  const char *textbase;
  svn_boolean_t replaced;
  svn_wc__db_status_t status;
  svn_revnum_t original_revision;
  const char *original_repos_relpath;
  svn_revnum_t revision;
  svn_revnum_t revert_base_revnum;
  svn_boolean_t have_base;
  svn_wc__db_status_t base_status;
  svn_boolean_t use_base = FALSE;

  SVN_ERR_ASSERT(! eb->diff_pristine);

  /* If the item is not a member of a specified changelist (and there are
     some specified changelists), skip it. */
  if (! svn_wc__internal_changelist_match(db, local_abspath,
                                          eb->changelist_hash, scratch_pool))
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc__db_read_info(&status, NULL, &revision, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL,
                               &original_repos_relpath, NULL, NULL,
                               &original_revision, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL,
                               NULL, &have_base, NULL, NULL,
                               db, local_abspath, scratch_pool, scratch_pool));
  if (have_base)
    SVN_ERR(svn_wc__db_base_get_info(&base_status, NULL, &revert_base_revnum,
                                     NULL, NULL, NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                     db, local_abspath,
                                     scratch_pool, scratch_pool));

  replaced = ((status == svn_wc__db_status_added)
              && have_base
              && base_status != svn_wc__db_status_not_present);

  /* A wc-wc diff of replaced files actually shows a diff against the
   * revert-base, showing all previous lines as removed and adding all new
   * lines. This does not happen for copied/moved-here files, not even with
   * show_copies_as_adds == TRUE (in which case copy/move is really shown as
   * an add, diffing against the empty file).
   * So show the revert-base revision for plain replaces. */
  if (replaced
      && ! original_repos_relpath)
    {
      use_base = TRUE;
      revision = revert_base_revnum;
    }

  /* Set TEXTBASE to the path to the text-base file that we want to diff
     against.

     ### There shouldn't be cases where the result is NULL, but at present
     there might be - see get_nearest_pristine_text_as_file(). */
  SVN_ERR(get_pristine_file(&textbase, db, local_abspath,
                            use_base, scratch_pool, scratch_pool));

  /* Delete compares text-base against empty file, modifications to the
   * working-copy version of the deleted file are not wanted.
   * Replace is treated like a delete plus an add: two comparisons are
   * generated, first one for the delete and then one for the add.
   * However, if this file was replaced and we are ignoring ancestry,
   * report it as a normal file modification instead. */
  if ((! replaced && status == svn_wc__db_status_deleted) ||
      (replaced && ! eb->ignore_ancestry))
    {
      apr_hash_t *left_props;
      void *file_baton = NULL;
      svn_boolean_t skip = FALSE;
      svn_diff_source_t *left_src = svn_diff__source_create(revision,
                                                            scratch_pool);

      /* Get svn:mime-type from pristine props (in BASE or WORKING) of PATH. */
      SVN_ERR(svn_wc__db_read_pristine_props(&left_props, db, local_abspath,
                                             scratch_pool, scratch_pool));

      SVN_ERR(eb->processor->file_opened(&file_baton,
                                         &skip,
                                         path,
                                         left_src,
                                         NULL /* right_source */,
                                         NULL /* copyfrom_source */,
                                         dir_baton,
                                         eb->processor,
                                         scratch_pool,
                                         scratch_pool));

      if (!skip)
        SVN_ERR(eb->processor->file_deleted(path,
                                            left_src,
                                            textbase,
                                            left_props,
                                            file_baton,
                                            eb->processor,
                                            scratch_pool));

      if (! (replaced && ! eb->ignore_ancestry))
        {
          /* We're here only for showing a delete, so we're done. */
          return SVN_NO_ERROR;
        }
    }

 /* Now deal with showing additions, or the add-half of replacements.
  * If the item is schedule-add *with history*, then we usually want
  * to see the usual working vs. text-base comparison, which will show changes
  * made since the file was copied.  But in case we're showing copies as adds,
  * we need to compare the copied file to the empty file. If we're doing a git
  * diff, and the file was copied, we need to report the file as added and
  * diff it against the text base, so that a "copied" git diff header, and
  * possibly a diff against the copy source, will be generated for it. */
  if (status == svn_wc__db_status_added
      && !(eb->ignore_ancestry && replaced))
    {
      void *file_baton = NULL;
      svn_boolean_t skip = FALSE;
      const char *translated = NULL;
      svn_diff_source_t *copyfrom_src = NULL;
      svn_diff_source_t *right_src = svn_diff__source_create(
                                                    SVN_INVALID_REVNUM,
                                                    scratch_pool);

      if (original_repos_relpath)
        {
          copyfrom_src = svn_diff__source_create(original_revision,
                                                 scratch_pool);
          copyfrom_src->repos_relpath = original_repos_relpath;
        }

      SVN_ERR(eb->processor->file_opened(&file_baton, &skip,
                                         path,
                                         NULL /* left source */,
                                         right_src,
                                         copyfrom_src,
                                         dir_baton,
                                         eb->processor,
                                         scratch_pool, scratch_pool));

      if (!skip)
        {
          apr_hash_t *right_props;
          apr_hash_t *copyfrom_props = NULL;

          /* Get svn:mime-type from ACTUAL props of PATH. */
          SVN_ERR(svn_wc__db_read_props(&right_props, db, local_abspath,
                                        scratch_pool, scratch_pool));

          if (copyfrom_src)
            {
              SVN_ERR(svn_wc__db_read_pristine_props(&copyfrom_props,
                                                     db, local_abspath,
                                                     scratch_pool,
                                                     scratch_pool));
            }

          SVN_ERR(svn_wc__internal_translated_file(&translated, local_abspath,
                                                   db, local_abspath,
                                                   SVN_WC_TRANSLATE_TO_NF
                                             | SVN_WC_TRANSLATE_USE_GLOBAL_TMP,
                                                   eb->cancel_func,
                                                   eb->cancel_baton,
                                                   scratch_pool,
                                                   scratch_pool));

          SVN_ERR(eb->processor->file_added(path,
                                            copyfrom_src,
                                            right_src,
                                            copyfrom_src
                                                ? textbase
                                                : NULL,
                                            translated,
                                            copyfrom_props,
                                            right_props,
                                            file_baton,
                                            eb->processor,
                                            scratch_pool));
        }
    }
  else
    {
      const char *translated = NULL;
      apr_hash_t *left_props;
      apr_hash_t *right_props;
      apr_array_header_t *propchanges;
      svn_boolean_t modified;
      void *file_baton = NULL;
      svn_boolean_t skip = FALSE;
      svn_diff_source_t *left_src = svn_diff__source_create(revision,
                                                            scratch_pool);
      svn_diff_source_t *right_src = svn_diff__source_create(
                                                    SVN_INVALID_REVNUM,
                                                    scratch_pool);

      SVN_ERR(eb->processor->file_opened(&file_baton, &skip,
                                         path,
                                         left_src,
                                         right_src,
                                         NULL,
                                         dir_baton,
                                         eb->processor,
                                         scratch_pool, scratch_pool));

      if (skip)
        return SVN_NO_ERROR;

      /* Here we deal with showing pure modifications. */
      SVN_ERR(svn_wc__internal_file_modified_p(&modified, db, local_abspath,
                                               FALSE, scratch_pool));
      if (modified)
        {
          /* Note that this might be the _second_ time we translate
             the file, as svn_wc__text_modified_internal_p() might have used a
             tmp translated copy too.  But what the heck, diff is
             already expensive, translating twice for the sake of code
             modularity is liveable. */
          SVN_ERR(svn_wc__internal_translated_file(
                    &translated, local_abspath, db, local_abspath,
                    SVN_WC_TRANSLATE_TO_NF | SVN_WC_TRANSLATE_USE_GLOBAL_TMP,
                    eb->cancel_func, eb->cancel_baton,
                    scratch_pool, scratch_pool));
        }

      /* Get the properties, the svn:mime-type values, and compute the
         differences between the two.  */
      if (replaced
          && eb->ignore_ancestry)
        {
          /* We don't want the normal pristine properties (which are
             from the WORKING tree). We want the pristines associated
             with the BASE tree, which are saved as "revert" props.  */
          SVN_ERR(svn_wc__db_base_get_props(&left_props,
                                            db, local_abspath,
                                            scratch_pool, scratch_pool));
        }
      else
        {
          /* We can only fetch the pristine props (from BASE or WORKING) if
             the node has not been replaced, or it was copied/moved here.  */
          SVN_ERR_ASSERT(!replaced
                         || status == svn_wc__db_status_copied
                         || status == svn_wc__db_status_moved_here);

          SVN_ERR(svn_wc__db_read_pristine_props(&left_props, db, local_abspath,
                                                 scratch_pool, scratch_pool));

          /* baseprops will be NULL for added nodes */
          if (!left_props)
            left_props = apr_hash_make(scratch_pool);
        }

      SVN_ERR(svn_wc__get_actual_props(&right_props, db, local_abspath,
                                       scratch_pool, scratch_pool));

      SVN_ERR(svn_prop_diffs(&propchanges, right_props, left_props,
                             scratch_pool));

      if (modified || propchanges->nelts > 0)
        {
          SVN_ERR(eb->processor->file_changed(path,
                                              left_src,
                                              right_src,
                                              textbase,
                                              translated,
                                              left_props,
                                              right_props,
                                              modified,
                                              propchanges,
                                              file_baton,
                                              eb->processor,
                                              scratch_pool));
        }
      else
        SVN_ERR(eb->processor->file_closed(path,
                                           left_src,
                                           right_src,
                                           file_baton,
                                           eb->processor,
                                           scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Called when the directory is closed to compare any elements that have
 * not yet been compared.  This identifies local, working copy only
 * changes.  At this stage we are dealing with files/directories that do
 * exist in the working copy.
 *
 * DIR_BATON is the baton for the directory.
 */
static svn_error_t *
walk_local_nodes_diff(struct edit_baton_t *eb,
                      const char *local_abspath,
                      const char *path,
                      svn_depth_t depth,
                      apr_hash_t *compared,
                      void *parent_baton,
                      apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = eb->db;
  svn_boolean_t in_anchor_not_target;
  apr_pool_t *iterpool;
  void *dir_baton = NULL;
  svn_boolean_t skip = FALSE;
  svn_boolean_t skip_children = FALSE;
  svn_revnum_t revision;
  svn_boolean_t props_mod;
  svn_diff_source_t *left_src;
  svn_diff_source_t *right_src;

  /* Everything we do below is useless if we are comparing to BASE. */
  if (eb->diff_pristine)
    return SVN_NO_ERROR;

  /* Determine if this is the anchor directory if the anchor is different
     to the target. When the target is a file, the anchor is the parent
     directory and if this is that directory the non-target entries must be
     skipped. */
  in_anchor_not_target = ((*path == '\0') && (*eb->target != '\0'));

  iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(svn_wc__db_read_info(NULL, NULL, &revision, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, &props_mod, NULL, NULL, NULL,
                               db, local_abspath, scratch_pool, scratch_pool));

  left_src = svn_diff__source_create(revision, scratch_pool);
  right_src = svn_diff__source_create(0, scratch_pool);

  if (!in_anchor_not_target)
    SVN_ERR(eb->processor->dir_opened(&dir_baton, &skip, &skip_children,
                                      path,
                                      left_src,
                                      right_src,
                                      NULL /* copyfrom_src */,
                                      parent_baton,
                                      eb->processor,
                                      scratch_pool, scratch_pool));


  if (!skip_children && depth != svn_depth_empty)
    {
      const apr_array_header_t *children;
      int i;
      SVN_ERR(svn_wc__db_read_children(&children, db, local_abspath,
                                       scratch_pool, iterpool));

      for (i = 0; i < children->nelts; i++)
        {
          const char *name = APR_ARRAY_IDX(children, i, const char*);
          const char *child_abspath, *child_path;
          svn_wc__db_status_t status;
          svn_kind_t kind;

          svn_pool_clear(iterpool);

          if (eb->cancel_func)
            SVN_ERR(eb->cancel_func(eb->cancel_baton));

          /* In the anchor directory, if the anchor is not the target then all
             entries other than the target should not be diff'd. Running diff
             on one file in a directory should not diff other files in that
             directory. */
          if (in_anchor_not_target && strcmp(eb->target, name))
            continue;

          child_abspath = svn_dirent_join(local_abspath, name, iterpool);

          SVN_ERR(svn_wc__db_read_info(&status, &kind, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       db, child_abspath,
                                       iterpool, iterpool));

          if (status == svn_wc__db_status_not_present
              || status == svn_wc__db_status_excluded
              || status == svn_wc__db_status_server_excluded)
            continue;

          child_path = svn_relpath_join(path, name, iterpool);

          /* Skip this node if it is in the list of nodes already diff'd. */
          if (compared && apr_hash_get(compared, child_path, APR_HASH_KEY_STRING))
            continue;

          switch (kind)
            {
            case svn_kind_file:
            case svn_kind_symlink:
              SVN_ERR(file_diff(eb, child_abspath, child_path, dir_baton,
                                iterpool));
              break;

            case svn_kind_dir:
              /* ### TODO: Don't know how to do replaced dirs. How do I get
                 information about what is being replaced? If it was a
                 directory then the directory elements are also going to be
                 deleted. We need to show deletion diffs for these
                 files. If it was a file we need to show a deletion diff
                 for that file. */

              /* Check the subdir if in the anchor (the subdir is the target),
                 or if recursive */
              if (in_anchor_not_target
                  || (depth > svn_depth_files)
                  || (depth == svn_depth_unknown))
                {
                  svn_depth_t depth_below_here = depth;

                  if (depth_below_here == svn_depth_immediates)
                    depth_below_here = svn_depth_empty;

                  SVN_ERR(walk_local_nodes_diff(eb,
                                                child_abspath,
                                                child_path,
                                                depth_below_here,
                                                NULL,
                                                dir_baton,
                                                iterpool));
                }
              break;

            default:
              break;
          }
        }
    }

    /* Check for local property mods on this directory, if we haven't
     already reported them and we aren't changelist-filted.
     ### it should be noted that we do not currently allow directories
     ### to be part of changelists, so if a changelist is provided, the
     ### changelist check will always fail. */
  if (! eb->changelist_hash
      && ! in_anchor_not_target
      && (!compared || ! svn_hash_gets(compared, path))
      && props_mod
      && ! skip)
    {
      apr_array_header_t *propchanges;
      apr_hash_t *left_props;
      apr_hash_t *right_props;

      SVN_ERR(svn_wc__internal_propdiff(&propchanges, &left_props,
                                        db, local_abspath,
                                        scratch_pool, scratch_pool));

      right_props = svn_prop__patch(left_props, propchanges, scratch_pool);

      SVN_ERR(eb->processor->dir_changed(path,
                                         left_src,
                                         right_src,
                                         left_props,
                                         right_props,
                                         propchanges,
                                         dir_baton,
                                         eb->processor,
                                         scratch_pool));
    }
  else if (! skip)
    SVN_ERR(eb->processor->dir_closed(path,
                                      left_src,
                                      right_src,
                                      dir_baton,
                                      eb->processor,
                                      scratch_pool));

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Report an existing file in the working copy (either in BASE or WORKING)
 * as having been added.
 *
 * Do all allocation in POOL.
 */
static svn_error_t *
report_wc_file_as_added(struct edit_baton_t *eb,
                        const char *local_abspath,
                        const char *path,
                        void *parent_baton,
                        apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = eb->db;
  svn_diff_source_t *right_src;
  const char *changelist;
  apr_hash_t *right_props = NULL;
  const char *source_file;
  const char *translated_file;
  svn_wc__db_status_t status;
  svn_revnum_t revision;
  void *file_baton = NULL;
  svn_boolean_t skip = FALSE;

  SVN_ERR(svn_wc__db_read_info(&status, NULL, &revision, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, &changelist,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath,
                               scratch_pool, scratch_pool));

  if (changelist && eb->changelist_hash
      && !svn_hash_gets(eb->changelist_hash, changelist))
    return SVN_NO_ERROR;

  if (status == svn_wc__db_status_added)
    SVN_ERR(svn_wc__db_scan_addition(&status, NULL, NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL, NULL, db,
                                     local_abspath, scratch_pool, scratch_pool));

  /* We can't show additions for files that don't exist. */
  SVN_ERR_ASSERT(status != svn_wc__db_status_deleted || eb->diff_pristine);

  /* If the file was added *with history*, then we don't want to
     see a comparison to the empty file;  we want the usual working
     vs. text-base comparison. */
  if (status == svn_wc__db_status_copied ||
      status == svn_wc__db_status_moved_here)
    {
      /* Don't show anything if we're comparing to BASE, since by
         definition there can't be any local modifications. */
      if (eb->diff_pristine)
        return SVN_NO_ERROR;

      /* Otherwise show just the local modifications. */
      return file_diff(eb, local_abspath, path, parent_baton, scratch_pool);
    }

  right_src = svn_diff__source_create(revision, scratch_pool);

  SVN_ERR(eb->processor->file_opened(&file_baton, &skip,
                                     path,
                                     NULL,
                                     right_src,
                                     NULL,
                                     parent_baton,
                                     eb->processor,
                                     scratch_pool, scratch_pool));

  if (skip)
    return SVN_NO_ERROR;

  if (eb->diff_pristine)
    SVN_ERR(svn_wc__db_read_pristine_props(&right_props, db, local_abspath,
                                           scratch_pool, scratch_pool));
  else
    SVN_ERR(svn_wc__db_read_props(&right_props, db, local_abspath,
                                  scratch_pool, scratch_pool));

  if (eb->diff_pristine)
    {
      SVN_ERR(get_pristine_file(&source_file, db, local_abspath,
                                FALSE, scratch_pool, scratch_pool));
      translated_file = source_file; /* No translation needed */
    }
  else
    {
      source_file = local_abspath;

      SVN_ERR(svn_wc__internal_translated_file(
           &translated_file, source_file, db, local_abspath,
           SVN_WC_TRANSLATE_TO_NF | SVN_WC_TRANSLATE_USE_GLOBAL_TMP,
           eb->cancel_func, eb->cancel_baton,
           scratch_pool, scratch_pool));
    }

  SVN_ERR(eb->processor->file_added(path,
                                    NULL /* copyfrom source */,
                                    right_src,
                                    NULL /* copyfrom file */,
                                    translated_file,
                                    NULL /* copyfrom props */,
                                    right_props,
                                    file_baton,
                                    eb->processor,
                                    scratch_pool));

  return SVN_NO_ERROR;
}

/* Report an existing directory in the working copy (either in BASE
 * or WORKING) as having been added.  If recursing, also report any
 * subdirectories as added.
 *
 * DIR_BATON is the baton for the directory.
 *
 * Do all allocation in POOL.
 */
static svn_error_t *
report_wc_directory_as_added(struct edit_baton_t *eb,
                             const char *local_abspath,
                             const char *path,
                             svn_depth_t depth,
                             void *parent_baton,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = eb->db;
  const apr_array_header_t *children;
  int i;
  apr_pool_t *iterpool;
  void *pdb = NULL;
  svn_boolean_t skip = FALSE;
  svn_boolean_t skip_children = FALSE;
  svn_diff_source_t *right_src = svn_diff__source_create(SVN_INVALID_REVNUM,
                                                         scratch_pool);

  /* Report the addition of the directory's contents. */
  iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(eb->processor->dir_opened(&pdb, &skip, &skip_children,
                                    path,
                                    NULL,
                                    right_src,
                                    NULL /* copyfrom_src */,
                                    parent_baton,
                                    eb->processor,
                                    scratch_pool, scratch_pool));


  SVN_ERR(svn_wc__db_read_children(&children, db, local_abspath,
                                   scratch_pool, iterpool));

  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);
      const char *child_abspath, *child_path;
      svn_wc__db_status_t status;
      svn_kind_t kind;

      svn_pool_clear(iterpool);

      if (eb->cancel_func)
        SVN_ERR(eb->cancel_func(eb->cancel_baton));

      child_abspath = svn_dirent_join(local_abspath, name, iterpool);

      SVN_ERR(svn_wc__db_read_info(&status, &kind, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   db, child_abspath, iterpool, iterpool));

      if (status == svn_wc__db_status_not_present
          || status == svn_wc__db_status_excluded
          || status == svn_wc__db_status_server_excluded)
        {
          continue;
        }

      /* If comparing against WORKING, skip entries that are
         schedule-deleted - they don't really exist. */
      if (!eb->diff_pristine && status == svn_wc__db_status_deleted)
        continue;

      child_path = svn_relpath_join(path, name, iterpool);

      switch (kind)
        {
        case svn_kind_file:
        case svn_kind_symlink:
          SVN_ERR(report_wc_file_as_added(eb, child_abspath, child_path,
                                          pdb, iterpool));
          break;

        case svn_kind_dir:
          if (depth > svn_depth_files || depth == svn_depth_unknown)
            {
              svn_depth_t depth_below_here = depth;

              if (depth_below_here == svn_depth_immediates)
                depth_below_here = svn_depth_empty;

              SVN_ERR(report_wc_directory_as_added(eb,
                                                   child_abspath,
                                                   child_path,
                                                   depth_below_here,
                                                   pdb,
                                                   iterpool));
            }
          break;

        default:
          break;
        }
    }

  if (!skip)
    {
      apr_hash_t *right_props;
      if (eb->diff_pristine)
        SVN_ERR(svn_wc__db_read_pristine_props(&right_props, db, local_abspath,
                                               scratch_pool, scratch_pool));
      else
        SVN_ERR(svn_wc__get_actual_props(&right_props, db, local_abspath,
                                         scratch_pool, scratch_pool));

      SVN_ERR(eb->processor->dir_added(path,
                                       NULL /* copyfrom_src */,
                                       right_src,
                                       NULL,
                                       right_props,
                                       pdb,
                                       eb->processor,
                                       iterpool));
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
set_target_revision(void *edit_baton,
                    svn_revnum_t target_revision,
                    apr_pool_t *pool)
{
  struct edit_baton_t *eb = edit_baton;
  eb->revnum = target_revision;

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. The root of the comparison hierarchy */
static svn_error_t *
open_root(void *edit_baton,
          svn_revnum_t base_revision,
          apr_pool_t *dir_pool,
          void **root_baton)
{
  struct edit_baton_t *eb = edit_baton;
  struct dir_baton_t *db;

  eb->root_opened = TRUE;
  db = make_dir_baton("", NULL, eb, FALSE, eb->depth, dir_pool);
  *root_baton = db;

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
delete_entry(const char *path,
             svn_revnum_t base_revision,
             void *parent_baton,
             apr_pool_t *pool)
{
  struct dir_baton_t *pb = parent_baton;
  struct edit_baton_t *eb = pb->eb;
  svn_wc__db_t *db = eb->db;
  const char *name = svn_dirent_basename(path, NULL);
  const char *local_abspath = svn_dirent_join(pb->local_abspath, name, pool);
  svn_wc__db_status_t status;
  svn_kind_t kind;

  /* Mark this node as compared in the parent directory's baton. */
  svn_hash_sets(pb->compared, apr_pstrdup(pb->pool, path), "");

  SVN_ERR(svn_wc__db_read_info(&status, &kind, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath, pool, pool));

  /* If comparing against WORKING, skip nodes that are deleted
     - they don't really exist. */
  if (!eb->diff_pristine && status == svn_wc__db_status_deleted)
    return SVN_NO_ERROR;

  switch (kind)
    {
    case svn_kind_file:
    case svn_kind_symlink:
      /* A delete is required to change working-copy into requested
         revision, so diff should show this as an add. Thus compare
         the empty file against the current working copy. */
          SVN_ERR(report_wc_file_as_added(eb, local_abspath, path,
                                          NULL /* ### parent_baton */,
                                          pool));
      break;
    case svn_kind_dir:
      /* A delete is required to change working-copy into requested
         revision, so diff should show this as an add. */
      SVN_ERR(report_wc_directory_as_added(eb,
                                           local_abspath,
                                           path,
                                           svn_depth_infinity,
                                           NULL /* ### parent_baton */,
                                           pool));

    default:
      break;
    }

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
add_directory(const char *path,
              void *parent_baton,
              const char *copyfrom_path,
              svn_revnum_t copyfrom_revision,
              apr_pool_t *dir_pool,
              void **child_baton)
{
  struct dir_baton_t *pb = parent_baton;
  struct edit_baton_t *eb = pb->eb;
  struct dir_baton_t *db;
  svn_depth_t subdir_depth = (pb->depth == svn_depth_immediates)
                              ? svn_depth_empty : pb->depth;

  /* ### TODO: support copyfrom? */

  db = make_dir_baton(path, pb, pb->eb, TRUE, subdir_depth,
                      dir_pool);
  *child_baton = db;

  /* Issue #3797: Don't add this filename to the parent directory's list of
     elements that have been compared, to show local additions via the local
     diff. The repository node is unrelated from the working copy version
     (similar to not-present in the working copy) */

  db->left_src  = svn_diff__source_create(eb->revnum, db->pool);

  SVN_ERR(eb->processor->dir_opened(&db->pdb, &db->skip, &db->skip_children,
                                    db->path,
                                    db->left_src,
                                    NULL /* right_source */,
                                    NULL /* copyfrom src */,
                                    pb->pdb,
                                    eb->processor,
                                    db->pool, db->pool));

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
open_directory(const char *path,
               void *parent_baton,
               svn_revnum_t base_revision,
               apr_pool_t *dir_pool,
               void **child_baton)
{
  struct dir_baton_t *pb = parent_baton;
  struct edit_baton_t *eb = pb->eb;
  struct dir_baton_t *db;
  svn_depth_t subdir_depth = (pb->depth == svn_depth_immediates)
                              ? svn_depth_empty : pb->depth;

  /* Allocate path from the parent pool since the memory is used in the
     parent's compared hash */
  db = make_dir_baton(path, pb, pb->eb, FALSE, subdir_depth, dir_pool);
  *child_baton = db;

  db->left_src  = svn_diff__source_create(eb->revnum, db->pool);
  db->right_src = svn_diff__source_create(SVN_INVALID_REVNUM, db->pool);

  /* Add this path to the parent directory's list of elements that
     have been compared. */
  svn_hash_sets(pb->compared, apr_pstrdup(pb->pool, db->path), "");

  SVN_ERR(eb->processor->dir_opened(&db->pdb, &db->skip, &db->skip_children,
                                    db->path,
                                    db->left_src,
                                    db->right_src,
                                    NULL /* copyfrom src */,
                                    pb->pdb,
                                    eb->processor,
                                    db->pool, db->pool));

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function.  When a directory is closed, all the
 * directory elements that have been added or replaced will already have been
 * diff'd. However there may be other elements in the working copy
 * that have not yet been considered.  */
static svn_error_t *
close_directory(void *dir_baton,
                apr_pool_t *pool)
{
  struct dir_baton_t *db = dir_baton;
  struct edit_baton_t *eb = db->eb;
  apr_pool_t *scratch_pool = db->pool;
  svn_boolean_t reported_closed = FALSE;

  /* Mark the properties of this directory as having already been
     compared so that we know not to show any local modifications
     in walk_local_nodes_diff. */
  svn_hash_sets(db->compared, db->path, "");

  /* Report local modifications for this directory.  Skip added
     directories since they can only contain added elements, all of
     which have already been diff'd. */
  if (!db->added)
    SVN_ERR(walk_local_nodes_diff(eb,
                                  db->local_abspath,
                                  db->path,
                                  db->depth,
                                  db->compared,
                                  NULL /* ### parent_baton */,
                                  scratch_pool));


  /* Report the property changes on the directory itself, if necessary. */
  if (db->propchanges->nelts > 0)
    {
      /* The working copy properties at the base of the wc->repos comparison:
         either BASE or WORKING. */
      apr_hash_t *originalprops;

      if (db->added)
        {
          originalprops = apr_hash_make(scratch_pool);
        }
      else
        {
          if (db->eb->diff_pristine)
            {
              SVN_ERR(svn_wc__db_read_pristine_props(&originalprops,
                                                     eb->db, db->local_abspath,
                                                     scratch_pool,
                                                     scratch_pool));
            }
          else
            {
              apr_hash_t *base_props, *repos_props;

              SVN_ERR(svn_wc__get_actual_props(&originalprops,
                                               eb->db, db->local_abspath,
                                               scratch_pool, scratch_pool));

              /* Load the BASE and repository directory properties. */
              SVN_ERR(svn_wc__db_base_get_props(&base_props,
                                                eb->db, db->local_abspath,
                                                scratch_pool, scratch_pool));

              repos_props = svn_prop__patch(base_props, db->propchanges,
                                            scratch_pool);

              /* Recalculate b->propchanges as the change between WORKING
                 and repos. */
              SVN_ERR(svn_prop_diffs(&db->propchanges, repos_props,
                                     originalprops, scratch_pool));
            }
        }

      if (!db->added)
        {
          reverse_propchanges(originalprops, db->propchanges, db->pool);

          SVN_ERR(eb->processor->dir_changed(db->path,
                                             db->left_src,
                                             db->right_src,
                                             originalprops,
                                             svn_prop__patch(originalprops,
                                                             db->propchanges,
                                                             scratch_pool),
                                             db->propchanges,
                                             db->pdb,
                                             eb->processor,
                                             scratch_pool));
        }
      else
        {
          SVN_ERR(eb->processor->dir_deleted(db->path,
                                             db->left_src,
                                             svn_prop__patch(originalprops,
                                                             db->propchanges,
                                                             scratch_pool),
                                             db->pdb,
                                             eb->processor,
                                             scratch_pool));
        }
      reported_closed = TRUE;
    }

  /* Mark this directory as compared in the parent directory's baton,
     unless this is the root of the comparison. */
  if (!reported_closed)
    SVN_ERR(eb->processor->dir_closed(db->path,
                                      db->left_src,
                                      db->right_src,
                                      db->pdb,
                                      eb->processor,
                                      scratch_pool));

  SVN_ERR(maybe_done(db)); /* destroys scratch_pool */

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
add_file(const char *path,
         void *parent_baton,
         const char *copyfrom_path,
         svn_revnum_t copyfrom_revision,
         apr_pool_t *file_pool,
         void **file_baton)
{
  struct dir_baton_t *pb = parent_baton;
  struct edit_baton_t *eb = pb->eb;
  struct file_baton_t *fb;

  /* ### TODO: support copyfrom? */

  fb = make_file_baton(path, TRUE, pb, file_pool);
  *file_baton = fb;

  /* Issue #3797: Don't add this filename to the parent directory's list of
     elements that have been compared, to show local additions via the local
     diff. The repository node is unrelated from the working copy version
     (similar to not-present in the working copy) */

  fb->left_src = svn_diff__source_create(eb->revnum, fb->pool);
  fb->right_src = svn_diff__source_create(SVN_INVALID_REVNUM, fb->pool);

  SVN_ERR(eb->processor->file_opened(&fb->pfb, &fb->skip,
                                     fb->path,
                                     fb->left_src,
                                     fb->right_src,
                                     NULL /* copyfrom src */,
                                     pb->pdb,
                                     eb->processor,
                                     fb->pool, fb->pool));

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
open_file(const char *path,
          void *parent_baton,
          svn_revnum_t base_revision,
          apr_pool_t *file_pool,
          void **file_baton)
{
  struct dir_baton_t *pb = parent_baton;
  struct edit_baton_t *eb = pb->eb;
  struct file_baton_t *fb;

  fb = make_file_baton(path, FALSE, pb, file_pool);
  *file_baton = fb;

  /* Add this filename to the parent directory's list of elements that
     have been compared. */
  svn_hash_sets(pb->compared, apr_pstrdup(pb->pool, path), "");

  SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, &fb->base_checksum, NULL,
                                   NULL, NULL, NULL, NULL,
                                   eb->db, fb->local_abspath,
                                   fb->pool, fb->pool));

  fb->left_src = svn_diff__source_create(eb->revnum, fb->pool);
  fb->right_src = svn_diff__source_create(SVN_INVALID_REVNUM, fb->pool);

  SVN_ERR(eb->processor->file_opened(&fb->pfb, &fb->skip,
                                     fb->path,
                                     fb->left_src,
                                     fb->right_src,
                                     NULL /* copyfrom src */,
                                     pb->pdb,
                                     eb->processor,
                                     fb->pool, fb->pool));

  return SVN_NO_ERROR;
}

/* Baton for window_handler */
struct window_handler_baton
{
  struct file_baton_t *fb;

  /* APPLY_HANDLER/APPLY_BATON represent the delta applcation baton. */
  svn_txdelta_window_handler_t apply_handler;
  void *apply_baton;

  unsigned char result_digest[APR_MD5_DIGESTSIZE];
};

/* Do the work of applying the text delta. */
static svn_error_t *
window_handler(svn_txdelta_window_t *window,
               void *window_baton)
{
  struct window_handler_baton *whb = window_baton;
  struct file_baton_t *fb = whb->fb;

  SVN_ERR(whb->apply_handler(window, whb->apply_baton));

  if (!window)
    {
      fb->result_checksum = svn_checksum__from_digest_md5(whb->result_digest,
                                                          fb->pool);
    }

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
apply_textdelta(void *file_baton,
                const char *base_checksum,
                apr_pool_t *pool,
                svn_txdelta_window_handler_t *handler,
                void **handler_baton)
{
  struct file_baton_t *fb = file_baton;
  struct window_handler_baton *whb;
  struct edit_baton_t *eb = fb->eb;
  svn_stream_t *source;
  svn_stream_t *temp_stream;

  if (fb->base_checksum)
    SVN_ERR(svn_wc__db_pristine_read(&source, NULL,
                                     eb->db, fb->local_abspath,
                                     fb->base_checksum,
                                     pool, pool));
  else
    source = svn_stream_empty(pool);

  /* This is the file that will contain the pristine repository version. */
  SVN_ERR(svn_stream_open_unique(&temp_stream, &fb->temp_file_path, NULL,
                                 svn_io_file_del_on_pool_cleanup,
                                 fb->pool, fb->pool));

  whb = apr_pcalloc(fb->pool, sizeof(*whb));
  whb->fb = fb;

  svn_txdelta_apply(source, temp_stream,
                    whb->result_digest,
                    fb->path /* error_info */,
                    fb->pool,
                    &whb->apply_handler, &whb->apply_baton);

  *handler = window_handler;
  *handler_baton = whb;
  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function.  When the file is closed we have a temporary
 * file containing a pristine version of the repository file. This can
 * be compared against the working copy.
 *
 * Ignore TEXT_CHECKSUM.
 */
static svn_error_t *
close_file(void *file_baton,
           const char *expected_md5_digest,
           apr_pool_t *pool)
{
  struct file_baton_t *fb = file_baton;
  struct edit_baton_t *eb = fb->eb;
  svn_wc__db_t *db = eb->db;
  apr_pool_t *scratch_pool = fb->pool;
  svn_wc__db_status_t status;
  const char *original_repos_relpath;
  svn_revnum_t original_revision;
  svn_error_t *err;

  /* The BASE information */
  const svn_checksum_t *pristine_checksum;
  const char *pristine_file;
  apr_hash_t *pristine_props;

  /* The repository information; constructed from BASE + Changes */
  const char *repos_file;
  apr_hash_t *repos_props;
  svn_boolean_t had_props, props_mod;

  /* The path to the wc file: either a pristine or actual. */
  const char *localfile;
  svn_boolean_t modified;
  /* The working copy properties at the base of the wc->repos
     comparison: either BASE or WORKING. */
  apr_hash_t *originalprops;

  if (expected_md5_digest != NULL)
    {
      svn_checksum_t *expected_checksum;
      const svn_checksum_t *repos_checksum = fb->result_checksum;

      SVN_ERR(svn_checksum_parse_hex(&expected_checksum, svn_checksum_md5,
                                     expected_md5_digest, scratch_pool));

      if (repos_checksum == NULL)
        repos_checksum = fb->base_checksum;

      if (repos_checksum->kind != svn_checksum_md5)
        SVN_ERR(svn_wc__db_pristine_get_md5(&repos_checksum,
                                            eb->db, fb->local_abspath,
                                            repos_checksum,
                                            scratch_pool, scratch_pool));

      if (!svn_checksum_match(expected_checksum, repos_checksum))
        return svn_checksum_mismatch_err(
                            expected_checksum,
                            repos_checksum,
                            pool,
                            _("Checksum mismatch for '%s'"),
                            svn_dirent_local_style(fb->local_abspath,
                                                   scratch_pool));
    }

  err = svn_wc__db_read_info(&status, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, &pristine_checksum, NULL,
                             &original_repos_relpath,
                             NULL, NULL, &original_revision, NULL, NULL, NULL,
                             NULL, NULL, NULL, &had_props, &props_mod,
                             NULL, NULL, NULL,
                             db, fb->local_abspath,
                             scratch_pool, scratch_pool);
  if (fb->added
      && err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);
      status = svn_wc__db_status_not_present;
      pristine_checksum = NULL;
      had_props = FALSE;
      props_mod = FALSE;
      original_repos_relpath = NULL;
      original_revision = SVN_INVALID_REVNUM;
    }
  else
    SVN_ERR(err);

  if (fb->added)
    {
      pristine_props = apr_hash_make(scratch_pool);
      pristine_file = NULL;
    }
  else
    {
      if (status != svn_wc__db_status_normal)
        SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, NULL, NULL, NULL, NULL,
                                         NULL, NULL, NULL, NULL,
                                         &pristine_checksum,
                                         NULL, NULL,
                                         &had_props, NULL, NULL,
                                         db, fb->local_abspath,
                                         scratch_pool, scratch_pool));

      SVN_ERR(svn_wc__db_pristine_get_path(&pristine_file,
                                           db, fb->local_abspath,
                                           pristine_checksum,
                                           scratch_pool, scratch_pool));

      if (had_props)
        SVN_ERR(svn_wc__db_base_get_props(&pristine_props,
                                           db, fb->local_abspath,
                                           scratch_pool, scratch_pool));
      else
        pristine_props = apr_hash_make(scratch_pool);
    }

  repos_props = svn_prop__patch(pristine_props, fb->propchanges, scratch_pool);
  repos_file = fb->temp_file_path ? fb->temp_file_path : pristine_file;

  /* If the file isn't in the working copy (either because it was added
     in the BASE->repos diff or because we're diffing against WORKING
     and it was marked as schedule-deleted), we show either an addition
     or a deletion of the complete contents of the repository file,
     depending upon the direction of the diff. */
  if (eb->ignore_ancestry && status == svn_wc__db_status_added)
    {
        /* Add this filename to the parent directory's list of elements that
           have been compared. */
      svn_hash_sets(fb->parent_baton->compared,
                    apr_pstrdup(fb->parent_baton->pool, fb->path), "");
    }
  else if (fb->added
           || (!eb->diff_pristine && status == svn_wc__db_status_deleted))
    {
      svn_boolean_t skip = FALSE;

      svn_diff_source_t *left_src = svn_diff__source_create(eb->revnum,
                                                            scratch_pool);

      SVN_ERR(eb->processor->file_opened(&fb->pfb, &skip,
                                         fb->path,
                                         left_src,
                                         NULL /* right source */,
                                         NULL /* copyfrom source */,
                                         NULL /* ### dir_baton */,
                                         eb->processor,
                                         scratch_pool, scratch_pool));

      if (! skip)
        SVN_ERR(eb->processor->file_deleted(fb->path,
                                            left_src,
                                            repos_file,
                                            repos_props,
                                            fb->pfb,
                                            eb->processor,
                                            scratch_pool));

      return SVN_NO_ERROR;
    }

  /* If the file was locally added with history, and we want to show copies
   * as added, diff the file with the empty file. */
  if (original_repos_relpath && eb->show_copies_as_adds)
    {
      svn_boolean_t skip = FALSE;

      svn_diff_source_t *right_src = svn_diff__source_create(eb->revnum,
                                                             scratch_pool);

      /* ### This code path looks like an ugly hack. No normalization,
             nothing... */

      SVN_ERR(eb->processor->file_opened(&fb->pfb, &skip,
                                         fb->path,
                                         NULL /* left_source */,
                                         right_src,
                                         NULL /* copyfrom source */,
                                         NULL /* ### dir_baton */,
                                         eb->processor,
                                         scratch_pool, scratch_pool));

      if (! skip)
        {
          apr_hash_t *right_props;
          SVN_ERR(svn_wc__db_read_props(&right_props, db, fb->local_abspath,
                                        scratch_pool, scratch_pool));

          SVN_ERR(eb->processor->file_added(fb->path,
                                          NULL /* copyfrom_src */,
                                          right_src,
                                          NULL /* copyfrom file */,
                                          fb->local_abspath,
                                          NULL /* copyfrom props */,
                                          right_props,
                                          fb->pfb,
                                          eb->processor,
                                          scratch_pool));
        }
    }
  /* If we didn't see any content changes between the BASE and repository
     versions (i.e. we only saw property changes), then, if we're diffing
     against WORKING, we also need to check whether there are any local
     (BASE:WORKING) modifications. */
  modified = (fb->temp_file_path != NULL);
  if (!modified && !eb->diff_pristine)
    SVN_ERR(svn_wc__internal_file_modified_p(&modified, eb->db,
                                             fb->local_abspath,
                                             FALSE, scratch_pool));

  if (modified)
    {
      if (eb->diff_pristine)
        SVN_ERR(get_pristine_file(&localfile, eb->db, fb->local_abspath,
                                  FALSE, scratch_pool, scratch_pool));
      else
        /* a detranslated version of the working file */
        SVN_ERR(svn_wc__internal_translated_file(
                 &localfile, fb->local_abspath, eb->db, fb->local_abspath,
                 SVN_WC_TRANSLATE_TO_NF | SVN_WC_TRANSLATE_USE_GLOBAL_TMP,
                 eb->cancel_func, eb->cancel_baton,
                 scratch_pool, scratch_pool));
    }
  else
    localfile = repos_file = NULL;

  if (eb->diff_pristine)
    {
      originalprops = pristine_props;
    }
  else
    {
      SVN_ERR(svn_wc__get_actual_props(&originalprops,
                                       eb->db, fb->local_abspath,
                                       scratch_pool, scratch_pool));

      /* We have the repository properties in repos_props, and the
         WORKING properties in originalprops.  Recalculate
         fb->propchanges as the change between WORKING and repos. */
      SVN_ERR(svn_prop_diffs(&fb->propchanges,
                             repos_props, originalprops, scratch_pool));
    }

  if (localfile || fb->propchanges->nelts > 0)
    {
      reverse_propchanges(originalprops, fb->propchanges, scratch_pool);

      SVN_ERR(eb->processor->file_changed(fb->path,
                                          fb->left_src,
                                          fb->right_src,
                                          repos_file,
                                          localfile,
                                          originalprops,
                                          svn_prop__patch(originalprops,
                                                          fb->propchanges,
                                                          scratch_pool),
                                          localfile != NULL,
                                          fb->propchanges,
                                          fb->pfb,
                                          eb->processor,
                                          scratch_pool));
    }

  SVN_ERR(maybe_done(fb->parent_baton));
  svn_pool_destroy(fb->pool); /* destroys scratch_pool */
  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
change_file_prop(void *file_baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *pool)
{
  struct file_baton_t *fb = file_baton;
  svn_prop_t *propchange;
  svn_prop_kind_t propkind;

  propkind = svn_property_kind2(name);
  if (propkind == svn_prop_wc_kind)
    return SVN_NO_ERROR;
  else if (propkind == svn_prop_regular_kind)
    fb->has_propchange = TRUE;

  propchange = apr_array_push(fb->propchanges);
  propchange->name = apr_pstrdup(fb->pool, name);
  propchange->value = value ? svn_string_dup(value, fb->pool) : NULL;

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
change_dir_prop(void *dir_baton,
                const char *name,
                const svn_string_t *value,
                apr_pool_t *pool)
{
  struct dir_baton_t *db = dir_baton;
  svn_prop_t *propchange;
  svn_prop_kind_t propkind;

  propkind = svn_property_kind2(name);
  if (propkind == svn_prop_wc_kind)
    return SVN_NO_ERROR;
  else if (propkind == svn_prop_regular_kind)
    db->has_propchange = TRUE;

  propchange = apr_array_push(db->propchanges);
  propchange->name = apr_pstrdup(db->pool, name);
  propchange->value = value ? svn_string_dup(value, db->pool) : NULL;

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
close_edit(void *edit_baton,
           apr_pool_t *pool)
{
  struct edit_baton_t *eb = edit_baton;

  if (!eb->root_opened)
    {
      SVN_ERR(walk_local_nodes_diff(eb,
                                    eb->anchor_abspath,
                                    "",
                                    eb->depth,
                                    NULL,
                                    NULL /* No parent_baton */,
                                    eb->pool));
    }

  return SVN_NO_ERROR;
}

/* Public Interface */


/* Create a diff editor and baton. */
svn_error_t *
svn_wc__get_diff_editor(const svn_delta_editor_t **editor,
                        void **edit_baton,
                        svn_wc_context_t *wc_ctx,
                        const char *anchor_abspath,
                        const char *target,
                        svn_depth_t depth,
                        svn_boolean_t ignore_ancestry,
                        svn_boolean_t show_copies_as_adds,
                        svn_boolean_t use_git_diff_format,
                        svn_boolean_t use_text_base,
                        svn_boolean_t reverse_order,
                        svn_boolean_t server_performs_filtering,
                        const apr_array_header_t *changelist_filter,
                        const svn_wc_diff_callbacks4_t *callbacks,
                        void *callback_baton,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  struct edit_baton_t *eb;
  void *inner_baton;
  svn_delta_editor_t *tree_editor;
  const svn_delta_editor_t *inner_editor;
  struct svn_wc__shim_fetch_baton_t *sfb;
  svn_delta_shim_callbacks_t *shim_callbacks =
                                svn_delta_shim_callbacks_default(result_pool);

  SVN_ERR_ASSERT(svn_dirent_is_absolute(anchor_abspath));

  SVN_ERR(make_edit_baton(&eb,
                          wc_ctx->db,
                          anchor_abspath, target,
                          callbacks, callback_baton,
                          depth, ignore_ancestry, show_copies_as_adds,
                          use_git_diff_format,
                          use_text_base, reverse_order, changelist_filter,
                          cancel_func, cancel_baton,
                          result_pool));

  tree_editor = svn_delta_default_editor(eb->pool);

  tree_editor->set_target_revision = set_target_revision;
  tree_editor->open_root = open_root;
  tree_editor->delete_entry = delete_entry;
  tree_editor->add_directory = add_directory;
  tree_editor->open_directory = open_directory;
  tree_editor->close_directory = close_directory;
  tree_editor->add_file = add_file;
  tree_editor->open_file = open_file;
  tree_editor->apply_textdelta = apply_textdelta;
  tree_editor->change_file_prop = change_file_prop;
  tree_editor->change_dir_prop = change_dir_prop;
  tree_editor->close_file = close_file;
  tree_editor->close_edit = close_edit;

  inner_editor = tree_editor;
  inner_baton = eb;

  if (!server_performs_filtering
      && depth == svn_depth_unknown)
    SVN_ERR(svn_wc__ambient_depth_filter_editor(&inner_editor,
                                                &inner_baton,
                                                wc_ctx->db,
                                                anchor_abspath,
                                                target,
                                                inner_editor,
                                                inner_baton,
                                                result_pool));

  SVN_ERR(svn_delta_get_cancellation_editor(cancel_func,
                                            cancel_baton,
                                            inner_editor,
                                            inner_baton,
                                            editor,
                                            edit_baton,
                                            result_pool));

  sfb = apr_palloc(result_pool, sizeof(*sfb));
  sfb->db = wc_ctx->db;
  sfb->base_abspath = eb->anchor_abspath;
  sfb->fetch_base = TRUE;

  shim_callbacks->fetch_kind_func = svn_wc__fetch_kind_func;
  shim_callbacks->fetch_props_func = svn_wc__fetch_props_func;
  shim_callbacks->fetch_base_func = svn_wc__fetch_base_func;
  shim_callbacks->fetch_baton = sfb;


  SVN_ERR(svn_editor__insert_shims(editor, edit_baton, *editor, *edit_baton,
                                   NULL, NULL, shim_callbacks,
                                   result_pool, scratch_pool));

  return SVN_NO_ERROR;
}

/* Wrapping svn_wc_diff_callbacks4_t as svn_diff_tree_processor_t */

/* baton for the svn_diff_tree_processor_t wrapper */
typedef struct wc_diff_wrap_baton_t
{
  const svn_wc_diff_callbacks4_t *callbacks;
  void *callback_baton;

  svn_boolean_t walk_deleted_dirs;

  apr_pool_t *result_pool;
  const char *empty_file;

} wc_diff_wrap_baton_t;

static svn_error_t *
wrap_ensure_empty_file(wc_diff_wrap_baton_t *wb,
                       apr_pool_t *scratch_pool)
{
  if (wb->empty_file)
    return SVN_NO_ERROR;

  /* Create a unique file in the tempdir */
  SVN_ERR(svn_io_open_uniquely_named(NULL, &wb->empty_file, NULL, NULL, NULL,
                                     svn_io_file_del_on_pool_cleanup,
                                     wb->result_pool, scratch_pool));

  return SVN_NO_ERROR;
}

/* svn_diff_tree_processor_t function */
static svn_error_t *
wrap_dir_opened(void **new_dir_baton,
                svn_boolean_t *skip,
                svn_boolean_t *skip_children,
                const char *relpath,
                const svn_diff_source_t *left_source,
                const svn_diff_source_t *right_source,
                const svn_diff_source_t *copyfrom_source,
                void *parent_dir_baton,
                const svn_diff_tree_processor_t *processor,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;
  svn_boolean_t tree_conflicted = FALSE;

  /* Maybe store state and tree_conflicted in baton? */
  if (left_source != NULL)
    {
      /* Open for change or delete */
      SVN_ERR(wb->callbacks->dir_opened(&tree_conflicted, skip, skip_children,
                                        relpath,
                                        right_source
                                            ? right_source->revision
                                            : (left_source
                                                    ? left_source->revision
                                                    : SVN_INVALID_REVNUM),
                                        wb->callback_baton,
                                        scratch_pool));

      if (! right_source && !wb->walk_deleted_dirs)
        *skip_children = TRUE;
    }
  else /* left_source == NULL -> Add */
    {
      svn_wc_notify_state_t state = svn_wc_notify_state_inapplicable;
      SVN_ERR(wb->callbacks->dir_added(&state, &tree_conflicted,
                                       skip, skip_children,
                                       relpath,
                                       right_source->revision,
                                       copyfrom_source
                                            ? copyfrom_source->repos_relpath
                                            : NULL,
                                       copyfrom_source
                                            ? copyfrom_source->revision
                                            : SVN_INVALID_REVNUM,
                                       wb->callback_baton,
                                       scratch_pool));
    }

  *new_dir_baton = NULL;

  return SVN_NO_ERROR;
}

/* svn_diff_tree_processor_t function */
static svn_error_t *
wrap_dir_added(const char *relpath,
               const svn_diff_source_t *right_source,
               const svn_diff_source_t *copyfrom_source,
               /*const*/ apr_hash_t *copyfrom_props,
               /*const*/ apr_hash_t *right_props,
               void *dir_baton,
               const svn_diff_tree_processor_t *processor,
               apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;
  svn_boolean_t tree_conflicted = FALSE;
  svn_wc_notify_state_t state = svn_wc_notify_state_unknown;
  svn_wc_notify_state_t prop_state = svn_wc_notify_state_unknown;
  apr_hash_t *pristine_props = copyfrom_props;
  apr_array_header_t *prop_changes = NULL;

  if (right_props && apr_hash_count(right_props))
    {
      if (!pristine_props)
        pristine_props = apr_hash_make(scratch_pool);

      SVN_ERR(svn_prop_diffs(&prop_changes, right_props, pristine_props,
                             scratch_pool));

      SVN_ERR(wb->callbacks->dir_props_changed(&prop_state,
                                               &tree_conflicted,
                                               relpath,
                                               TRUE /* dir_was_added */,
                                               prop_changes, pristine_props,
                                               wb->callback_baton,
                                               scratch_pool));
    }

  SVN_ERR(wb->callbacks->dir_closed(&state, &prop_state,
                                   &tree_conflicted,
                                   relpath,
                                   TRUE /* dir_was_added */,
                                   wb->callback_baton,
                                   scratch_pool));
  return SVN_NO_ERROR;
}

/* svn_diff_tree_processor_t function */
static svn_error_t *
wrap_dir_deleted(const char *relpath,
                 const svn_diff_source_t *left_source,
                 /*const*/ apr_hash_t *left_props,
                 void *dir_baton,
                 const svn_diff_tree_processor_t *processor,
                 apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;
  svn_boolean_t tree_conflicted = FALSE;
  svn_wc_notify_state_t state = svn_wc_notify_state_inapplicable;

  SVN_ERR(wb->callbacks->dir_deleted(&state, &tree_conflicted,
                                     relpath,
                                     wb->callback_baton,
                                     scratch_pool));

  return SVN_NO_ERROR;
}

/* svn_diff_tree_processor_t function */
static svn_error_t *
wrap_dir_closed(const char *relpath,
                const svn_diff_source_t *left_source,
                const svn_diff_source_t *right_source,
                void *dir_baton,
                const svn_diff_tree_processor_t *processor,
                apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;

  /* No previous implementations provided these arguments, so we
     are not providing them either */
  SVN_ERR(wb->callbacks->dir_closed(NULL, NULL, NULL,
                                    relpath,
                                    (left_source == NULL) /* added */,
                                    wb->callback_baton,
                                    scratch_pool));

return SVN_NO_ERROR;
}

/* svn_diff_tree_processor_t function */
static svn_error_t *
wrap_dir_changed(const char *relpath,
                 const svn_diff_source_t *left_source,
                 const svn_diff_source_t *right_source,
                 /*const*/ apr_hash_t *left_props,
                 /*const*/ apr_hash_t *right_props,
                 const apr_array_header_t *prop_changes,
                 void *dir_baton,
                 const struct svn_diff_tree_processor_t *processor,
                 apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;
  svn_boolean_t tree_conflicted = FALSE;
  svn_wc_notify_state_t prop_state = svn_wc_notify_state_inapplicable;

  SVN_ERR(wb->callbacks->dir_props_changed(&prop_state, &tree_conflicted,
                                           relpath,
                                           (left_source == NULL) /* added */,
                                           prop_changes,
                                           left_props,
                                           wb->callback_baton,
                                           scratch_pool));

  /* And call dir_closed, etc */
  SVN_ERR(wrap_dir_closed(relpath, left_source, right_source,
                          dir_baton, processor,
                          scratch_pool));
  return SVN_NO_ERROR;
}

/* svn_diff_tree_processor_t function */
static svn_error_t *
wrap_file_opened(void **new_file_baton,
                 svn_boolean_t *skip,
                 const char *relpath,
                 const svn_diff_source_t *left_source,
                 const svn_diff_source_t *right_source,
                 const svn_diff_source_t *copyfrom_source,
                 void *dir_baton,
                 const svn_diff_tree_processor_t *processor,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;
  svn_boolean_t tree_conflicted = FALSE;

  if (left_source) /* If ! added */
    SVN_ERR(wb->callbacks->file_opened(&tree_conflicted, skip, relpath,
                                       right_source
                                            ? right_source->revision
                                            : (left_source
                                                    ? left_source->revision
                                                    : SVN_INVALID_REVNUM),
                                       wb->callback_baton, scratch_pool));

  /* No old implementation used the output arguments for notify */

  *new_file_baton = NULL;
  return SVN_NO_ERROR;
}

/* svn_diff_tree_processor_t function */
static svn_error_t *
wrap_file_added(const char *relpath,
                const svn_diff_source_t *copyfrom_source,
                const svn_diff_source_t *right_source,
                const char *copyfrom_file,
                const char *right_file,
                /*const*/ apr_hash_t *copyfrom_props,
                /*const*/ apr_hash_t *right_props,
                void *file_baton,
                const svn_diff_tree_processor_t *processor,
                apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;
  svn_boolean_t tree_conflicted = FALSE;
  svn_wc_notify_state_t state = svn_wc_notify_state_inapplicable;
  svn_wc_notify_state_t prop_state = svn_wc_notify_state_inapplicable;
  apr_array_header_t *prop_changes;

  if (! copyfrom_props)
    copyfrom_props = apr_hash_make(scratch_pool);

  SVN_ERR(svn_prop_diffs(&prop_changes, right_props, copyfrom_props,
                         scratch_pool));

  if (! copyfrom_source)
    SVN_ERR(wrap_ensure_empty_file(wb, scratch_pool));

  SVN_ERR(wb->callbacks->file_added(&state, &prop_state, &tree_conflicted,
                                    relpath,
                                    copyfrom_source
                                        ? copyfrom_file
                                        : wb->empty_file,
                                    right_file,
                                    copyfrom_source
                                       ? copyfrom_source->revision
                                       : 0 /* For legacy reasons */,
                                    right_source->revision,
                                    copyfrom_props
                                     ? svn_prop_get_value(copyfrom_props,
                                                          SVN_PROP_MIME_TYPE)
                                     : NULL,
                                    right_props
                                     ? svn_prop_get_value(right_props,
                                                          SVN_PROP_MIME_TYPE)
                                     : NULL,
                                    copyfrom_source
                                            ? copyfrom_source->repos_relpath
                                            : NULL,
                                    copyfrom_source
                                            ? copyfrom_source->revision
                                            : SVN_INVALID_REVNUM,
                                    prop_changes, copyfrom_props,
                                    wb->callback_baton,
                                    scratch_pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_file_deleted(const char *relpath,
                  const svn_diff_source_t *left_source,
                  const char *left_file,
                  apr_hash_t *left_props,
                  void *file_baton,
                  const svn_diff_tree_processor_t *processor,
                  apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;
  svn_boolean_t tree_conflicted = FALSE;
  svn_wc_notify_state_t state = svn_wc_notify_state_inapplicable;

  SVN_ERR(wrap_ensure_empty_file(wb, scratch_pool));

  SVN_ERR(wb->callbacks->file_deleted(&state, &tree_conflicted,
                                      relpath,
                                      left_file, wb->empty_file,
                                      left_props
                                       ? svn_prop_get_value(left_props,
                                                            SVN_PROP_MIME_TYPE)
                                       : NULL,
                                      NULL,
                                      left_props,
                                      wb->callback_baton,
                                      scratch_pool));
  return SVN_NO_ERROR;
}

/* svn_diff_tree_processor_t function */
static svn_error_t *
wrap_file_changed(const char *relpath,
                  const svn_diff_source_t *left_source,
                  const svn_diff_source_t *right_source,
                  const char *left_file,
                  const char *right_file,
                  /*const*/ apr_hash_t *left_props,
                  /*const*/ apr_hash_t *right_props,
                  svn_boolean_t file_modified,
                  const apr_array_header_t *prop_changes,
                  void *file_baton,
                  const svn_diff_tree_processor_t *processor,
                  apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wb = processor->baton;
  svn_boolean_t tree_conflicted = FALSE;
  svn_wc_notify_state_t state = svn_wc_notify_state_inapplicable;
  svn_wc_notify_state_t prop_state = svn_wc_notify_state_inapplicable;

  SVN_ERR(wrap_ensure_empty_file(wb, scratch_pool));

  SVN_ERR(wb->callbacks->file_changed(&state, &prop_state, &tree_conflicted,
                                      relpath,
                                      left_file, right_file,
                                      left_source->revision,
                                      right_source->revision,
                                      left_props
                                       ? svn_prop_get_value(left_props,
                                                            SVN_PROP_MIME_TYPE)
                                       : NULL,
                                      right_props
                                       ? svn_prop_get_value(right_props,
                                                            SVN_PROP_MIME_TYPE)
                                       : NULL,
                                       prop_changes,
                                      left_props,
                                      wb->callback_baton,
                                      scratch_pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__wrap_diff_callbacks(const svn_diff_tree_processor_t **diff_processor,
                            const svn_wc_diff_callbacks4_t *callbacks,
                            void *callback_baton,
                            svn_boolean_t walk_deleted_dirs,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  wc_diff_wrap_baton_t *wrap_baton;
  svn_diff_tree_processor_t *processor;

  wrap_baton = apr_pcalloc(result_pool, sizeof(*wrap_baton));

  wrap_baton->result_pool = result_pool;
  wrap_baton->callbacks = callbacks;
  wrap_baton->callback_baton = callback_baton;
  wrap_baton->empty_file = NULL;
  wrap_baton->walk_deleted_dirs = walk_deleted_dirs;

  processor = svn_diff__tree_processor_create(wrap_baton, result_pool);

  processor->dir_opened   = wrap_dir_opened;
  processor->dir_added    = wrap_dir_added;
  processor->dir_deleted  = wrap_dir_deleted;
  processor->dir_changed  = wrap_dir_changed;
  processor->dir_closed   = wrap_dir_closed;

  processor->file_opened   = wrap_file_opened;
  processor->file_added    = wrap_file_added;
  processor->file_deleted  = wrap_file_deleted;
  processor->file_changed  = wrap_file_changed;
  /*processor->file_closed   = wrap_file_closed*/; /* Not needed */

  *diff_processor = processor;
  return SVN_NO_ERROR;
}
