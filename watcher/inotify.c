/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef HAVE_INOTIFY_INIT

#ifndef IN_EXCL_UNLINK
/* defined in <linux/inotify.h> but we can't include that without
 * breaking userspace */
# define WATCHMAN_IN_EXCL_UNLINK 0x04000000
#else
# define WATCHMAN_IN_EXCL_UNLINK IN_EXCL_UNLINK
#endif

#define WATCHMAN_INOTIFY_MASK \
  IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | \
  IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | \
  IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR | WATCHMAN_IN_EXCL_UNLINK

struct pending_move {
  time_t created;
  w_string_t *name;
};

struct inot_root_state {
  /* we use one inotify instance per watched root dir */
  int infd;

  /* map of active watch descriptor to name of the corresponding dir */
  w_ht_t *wd_to_name;
  /* map of inotify cookie to corresponding name */
  w_ht_t *move_map;
  /* lock to protect both of the maps above */
  pthread_mutex_t lock;

  // Make the buffer big enough for 16k entries, which
  // happens to be the default fs.inotify.max_queued_events
  char ibuf[WATCHMAN_BATCH_LIMIT * (sizeof(struct inotify_event) + 256)];
};

static w_ht_val_t copy_pending(w_ht_val_t key) {
  struct pending_move *src = w_ht_val_ptr(key);
  struct pending_move *dest = malloc(sizeof(*dest));
  dest->created = src->created;
  dest->name = src->name;
  w_string_addref(src->name);
  return w_ht_ptr_val(dest);
}

static void del_pending(w_ht_val_t key) {
  struct pending_move *p = w_ht_val_ptr(key);

  w_string_delref(p->name);
  free(p);
}

static const struct watchman_hash_funcs move_hash_funcs = {
  NULL, // copy_key
  NULL, // del_key
  NULL, // equal_key
  NULL, // hash_key
  copy_pending, // copy_val
  del_pending   // del_val
};

watchman_global_watcher_t inot_global_init(void) {
  return NULL;
}

void inot_global_dtor(watchman_global_watcher_t watcher) {
  unused_parameter(watcher);
}

static const char *inot_strerror(int err) {
  switch (err) {
    case EMFILE:
      return "The user limit on the total number of inotify "
        "instances has been reached; increase the "
        "fs.inotify.max_user_instances sysctl";
    case ENFILE:
      return "The system limit on the total number of file descriptors "
        "has been reached";
    case ENOMEM:
      return "Insufficient kernel memory is available";
    case ENOSPC:
      return "The user limit on the total number of inotify watches "
        "was reached; increase the fs.inotify.max_user_watches sysctl";
    default:
      return strerror(err);
  }
}

bool inot_root_init(watchman_global_watcher_t watcher, w_root_t *root,
    char **errmsg) {
  struct inot_root_state *state;
  unused_parameter(watcher);

  state = calloc(1, sizeof(*state));
  if (!state) {
    *errmsg = strdup("out of memory");
    return false;
  }
  root->watch = state;
  pthread_mutex_init(&state->lock, NULL);

  state->infd = inotify_init();
  if (state->infd == -1) {
    ignore_result(asprintf(errmsg, "watch(%.*s): inotify_init error: %s",
        root->root_path->len, root->root_path->buf, inot_strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(state->infd);
  state->wd_to_name = w_ht_new(HINT_NUM_DIRS, &w_ht_string_val_funcs);
  state->move_map = w_ht_new(2, &move_hash_funcs);

  return true;
}

void inot_root_dtor(watchman_global_watcher_t watcher, w_root_t *root) {
  struct inot_root_state *state = root->watch;
  unused_parameter(watcher);

  if (!state) {
    return;
  }

  pthread_mutex_destroy(&state->lock);

  close(state->infd);
  state->infd = -1;
  if (state->wd_to_name) {
    w_ht_free(state->wd_to_name);
    state->wd_to_name = NULL;
  }
  if (state->move_map) {
    w_ht_free(state->move_map);
    state->move_map = NULL;
  }

  free(state);
  root->watch = NULL;
}

static void inot_root_signal_threads(watchman_global_watcher_t watcher,
    w_root_t *root) {
  unused_parameter(watcher);
  unused_parameter(root);
}

static bool inot_root_start(watchman_global_watcher_t watcher, w_root_t *root) {
  unused_parameter(watcher);
  unused_parameter(root);

  return true;
}

static bool inot_root_start_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(root);
  unused_parameter(file);
  return true;
}

static void inot_root_stop_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(root);
  unused_parameter(file);
}

static DIR *inot_root_start_watch_dir(watchman_global_watcher_t watcher,
    w_root_t *root, struct watchman_dir *dir, struct timeval now,
    const char *path) {
  struct inot_root_state *state = root->watch;
  DIR *osdir = NULL;
  int newwd;
  unused_parameter(watcher);

  // The directory might be different since the last time we looked at it, so
  // call inotify_add_watch unconditionally.
  newwd = inotify_add_watch(state->infd, path, WATCHMAN_INOTIFY_MASK);
  if (newwd == -1) {
    if (errno == ENOSPC || errno == ENOMEM) {
      // Limits exceeded, no recovery from our perspective
      set_poison_state(root, dir->path, now, "inotify-add-watch", errno,
          inot_strerror(errno));
    } else {
      handle_open_errno(root, dir, now, "inotify_add_watch", errno,
          inot_strerror(errno));
    }
    return NULL;
  }

  // record mapping
  pthread_mutex_lock(&state->lock);
  w_ht_replace(state->wd_to_name, newwd, w_ht_ptr_val(dir->path));
  pthread_mutex_unlock(&state->lock);
  w_log(W_LOG_DBG, "adding %d -> %s mapping\n", newwd, path);

  osdir = opendir_nofollow(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, NULL);
    return NULL;
  }

  return osdir;
}

static void inot_root_stop_watch_dir(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_dir *dir) {
  struct inot_root_state *state = root->watch;
  unused_parameter(watcher);
  unused_parameter(state);
  unused_parameter(root);
  unused_parameter(dir);

  // Linux removes watches for us at the appropriate times,
  // and tells us about it via inotify, so we have nothing to do here
}

static void process_inotify_event(
    w_root_t *root,
    struct watchman_pending_collection *coll,
    struct inotify_event *ine,
    struct timeval now)
{
  struct inot_root_state *state = root->watch;

  w_log(W_LOG_DBG, "notify: wd=%d mask=%x %s\n", ine->wd, ine->mask,
      ine->len > 0 ? ine->name : "");

  if (ine->wd == -1 && (ine->mask & IN_Q_OVERFLOW)) {
    /* we missed something, will need to re-crawl */
    w_root_schedule_recrawl(root, "IN_Q_OVERFLOW");
  } else if (ine->wd != -1) {
    w_string_t *dir_name = NULL;
    w_string_t *name = NULL;
    char buf[WATCHMAN_NAME_MAX];

    pthread_mutex_lock(&state->lock);
    dir_name = w_ht_val_ptr(w_ht_get(state->wd_to_name, ine->wd));
    if (dir_name) {
      w_string_addref(dir_name);
    }
    pthread_mutex_unlock(&state->lock);

    if (dir_name) {
      if (ine->len > 0) {
        snprintf(buf, sizeof(buf), "%.*s/%s",
            dir_name->len, dir_name->buf,
            ine->name);
        name = w_string_new(buf);
      } else {
        name = dir_name;
        w_string_addref(name);
      }
    }

    if (ine->len > 0 && (ine->mask & IN_MOVED_FROM)) {
      struct pending_move mv;

      // record this as a pending move, so that we can automatically
      // watch the target when we get the other side of it.
      mv.created = now.tv_sec;
      mv.name = name;

      pthread_mutex_lock(&state->lock);
      if (!w_ht_replace(state->move_map, ine->cookie, w_ht_ptr_val(&mv))) {
        w_log(W_LOG_FATAL,
            "failed to store %" PRIx32 " -> %s in move map\n",
            ine->cookie, name->buf);
      }
      pthread_mutex_unlock(&state->lock);

      w_log(W_LOG_DBG,
          "recording move_from %" PRIx32 " %s\n", ine->cookie,
          name->buf);
    }

    if (ine->len > 0 && (ine->mask & IN_MOVED_TO)) {
      struct pending_move *old;

      pthread_mutex_lock(&state->lock);
      old = w_ht_val_ptr(w_ht_get(state->move_map, ine->cookie));
      if (old) {
        int wd = inotify_add_watch(state->infd, name->buf, WATCHMAN_INOTIFY_MASK);
        if (wd == -1) {
          if (errno == ENOSPC || errno == ENOMEM) {
            // Limits exceeded, no recovery from our perspective
            set_poison_state(root, name, now, "inotify-add-watch", errno,
                inot_strerror(errno));
          } else {
            w_log(W_LOG_DBG, "add_watch: %s %s\n",
                name->buf, inot_strerror(errno));
          }
        } else {
          w_log(W_LOG_DBG, "moved %s -> %s\n", old->name->buf, name->buf);
          w_ht_replace(state->wd_to_name, wd, w_ht_ptr_val(name));
        }
      } else {
        w_log(W_LOG_DBG, "move: cookie=%" PRIx32 " not found in move map %s\n",
            ine->cookie, name->buf);
      }
      pthread_mutex_unlock(&state->lock);
    }

    if (dir_name) {
      if ((ine->mask & (IN_UNMOUNT|IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF))) {
        w_string_t *pname;

        if (w_string_equal(root->root_path, name)) {
          w_log(W_LOG_ERR,
              "root dir %s has been (re)moved, canceling watch\n",
              root->root_path->buf);
          w_string_delref(name);
          w_string_delref(dir_name);
          w_root_cancel(root);
          return;
        }

        // We need to examine the parent and crawl down
        pname = w_string_dirname(name);
        w_log(W_LOG_DBG, "mask=%x, focus on parent: %.*s\n",
            ine->mask, pname->len, pname->buf);
        w_string_delref(name);
        name = pname;
      }

      w_log(W_LOG_DBG, "add_pending for inotify mask=%x %.*s\n",
          ine->mask, name->len, name->buf);
      w_pending_coll_add(coll, name, true, now, true);

      w_string_delref(name);

      // The kernel removed the wd -> name mapping, so let's update
      // our state here also
      if ((ine->mask & IN_IGNORED) != 0) {
        w_log(W_LOG_DBG, "mask=%x: remove watch %d %.*s\n", ine->mask,
            ine->wd, dir_name->len, dir_name->buf);
        pthread_mutex_lock(&state->lock);
        w_ht_del(state->wd_to_name, ine->wd);
        pthread_mutex_unlock(&state->lock);
      }

      w_string_delref(dir_name);

    } else if ((ine->mask & (IN_MOVE_SELF|IN_IGNORED)) == 0) {
      // If we can't resolve the dir, and this isn't notification
      // that it has gone away, then we want to recrawl to fix
      // up our state.
      w_log(W_LOG_ERR, "wanted dir %d for mask %x but not found %.*s\n",
          ine->wd, ine->mask, ine->len, ine->name);
      w_root_schedule_recrawl(root, "dir missing from internal state");
    }
  }
}

static bool inot_root_consume_notify(watchman_global_watcher_t watcher,
    w_root_t *root, struct watchman_pending_collection *coll)
{
  struct inot_root_state *state = root->watch;
  struct inotify_event *ine;
  char *iptr;
  int n;
  struct timeval now;
  unused_parameter(watcher);

  n = read(state->infd, &state->ibuf, sizeof(state->ibuf));
  if (n == -1) {
    if (errno == EINTR) {
      return false;
    }
    w_log(W_LOG_FATAL, "read(%d, %zu): error %s\n",
        state->infd, sizeof(state->ibuf), strerror(errno));
  }

  w_log(W_LOG_DBG, "inotify read: returned %d.\n", n);
  gettimeofday(&now, NULL);

  for (iptr = state->ibuf; iptr < state->ibuf + n;
      iptr = iptr + sizeof(*ine) + ine->len) {
    ine = (struct inotify_event*)iptr;

    process_inotify_event(root, coll, ine, now);

    if (root->cancelled) {
      return false;
    }
  }

  return true;
}

static bool inot_root_wait_notify(watchman_global_watcher_t watcher,
    w_root_t *root, int timeoutms) {
  struct inot_root_state *state = root->watch;
  int n;
  struct pollfd pfd;
  unused_parameter(watcher);

  pfd.fd = state->infd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static void inot_file_free(watchman_global_watcher_t watcher,
    struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(file);
}

struct watchman_ops inotify_watcher = {
  "inotify",
  true, // per_file_notifications
  inot_global_init,
  inot_global_dtor,
  inot_root_init,
  inot_root_start,
  inot_root_dtor,
  inot_root_start_watch_file,
  inot_root_stop_watch_file,
  inot_root_start_watch_dir,
  inot_root_stop_watch_dir,
  inot_root_signal_threads,
  inot_root_consume_notify,
  inot_root_wait_notify,
  inot_file_free
};

#endif // HAVE_INOTIFY_INIT

/* vim:ts=2:sw=2:et:
 */
