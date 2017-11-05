/*
 * Copyright (C) 2010-2011 Julien BLACHE <jb@jblache.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <regex.h>
#include <stdint.h>
#include <inttypes.h>

#ifdef HAVE_EVENTFD
# include <sys/eventfd.h>
#endif

#include <event2/event.h>

#include "httpd_dacp.h"
#include "httpd_daap.h"
#include "logger.h"
#include "misc.h"
#include "conffile.h"
#include "artwork.h"
#include "dmap_common.h"
#include "db.h"
#include "daap_query.h"
#include "player.h"
#include "listener.h"

/* httpd event base, from httpd.c */
extern struct event_base *evbase_httpd;

// Will be filled out by dacp_request_parse()
struct dacp_request {
  // The parsed request URI given to us by httpd.c
  struct httpd_uri_parsed *uri_parsed;
  // Shortcut to &uri_parsed->ev_query
  struct evkeyvalq *query;
  // http request struct
  struct evhttp_request *req;
  // A pointer to the handler that will process the request
  void (*handler)(struct evbuffer *reply, struct dacp_request *dreq);
};

struct uri_map {
  regex_t preg;
  char *regexp;
  void (*handler)(struct evbuffer *reply, struct dacp_request *dreq);
};

struct dacp_update_request {
  struct evhttp_request *req;

  struct dacp_update_request *next;
};

typedef void (*dacp_propget)(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
typedef void (*dacp_propset)(const char *value, struct evkeyvalq *query);

struct dacp_prop_map {
  char *desc;
  dacp_propget propget;
  dacp_propset propset;
};

/* Forward declaration of handlers */
static struct uri_map dacp_handlers[];

/* ---------------- FORWARD - PROPERTIES GETTERS AND SETTERS ---------------- */

static void
dacp_propget_volume(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_volumecontrollable(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_playerstate(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_shufflestate(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_availableshufflestates(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_repeatstate(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_availablerepeatstates(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_nowplaying(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_playingtime(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);

static void
dacp_propget_fullscreenenabled(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_fullscreen(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_visualizerenabled(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_visualizer(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_itms_songid(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_haschapterdata(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_mediakind(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);
static void
dacp_propget_extendedmediakind(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item);


static void
dacp_propset_volume(const char *value, struct evkeyvalq *query);
static void
dacp_propset_playingtime(const char *value, struct evkeyvalq *query);
static void
dacp_propset_shufflestate(const char *value, struct evkeyvalq *query);
static void
dacp_propset_repeatstate(const char *value, struct evkeyvalq *query);
static void
dacp_propset_userrating(const char *value, struct evkeyvalq *query);


/* gperf static hash, dacp_prop.gperf */
#include "dacp_prop_hash.h"


/* Play status update */
#ifdef HAVE_EVENTFD
static int update_efd;
#else
static int update_pipe[2];
#endif
static struct event *updateev;
/* Next revision number the client should call with */
static int current_rev;

/* Play status update requests */
static struct dacp_update_request *update_requests;

/* Seek timer */
static struct event *seek_timer;
static int seek_target;

/* If an item is removed from the library while in the queue, we replace it with this */
static struct media_file_info dummy_mfi =
{
  .id = 9999999,
  .title = "(unknown title)",
  .artist = "(unknown artist)",
  .album = "(unknown album)",
  .genre = "(unknown genre)",
};
static struct db_queue_item dummy_queue_item =
{
  .file_id = 9999999,
  .title = "(unknown title)",
  .artist = "(unknown artist)",
  .album = "(unknown album)",
  .genre = "(unknown genre)",
};


/* -------------------------------- HELPERS --------------------------------- */

static void
dacp_nowplaying(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  uint32_t id;
  int64_t songalbumid;
  int pos_pl;

  if ((status->status == PLAY_STOPPED) || !queue_item)
    return;

  /* Send bogus id's if playing internet radio, because clients like
   * Remote and Retune will only update metadata (like artwork) if the id's
   * change (which they wouldn't do if we sent the real ones)
   * FIXME: Giving the client invalid ids on purpose is hardly ideal, but the
   * clients don't seem to use these ids for anything other than rating.
   */
  if (queue_item->data_kind == DATA_KIND_HTTP)
    {
      id = djb_hash(queue_item->album, strlen(queue_item->album));
      songalbumid = (int64_t)id;
    }
  else
    {
      id = status->id;
      songalbumid = queue_item->songalbumid;
    }

  pos_pl = db_queue_get_pos(status->item_id, 0);

  dmap_add_container(evbuf, "canp", 16);
  dmap_add_raw_uint32(evbuf, 1); /* Database */
  dmap_add_raw_uint32(evbuf, status->plid);
  dmap_add_raw_uint32(evbuf, pos_pl);
  dmap_add_raw_uint32(evbuf, id);

  dmap_add_string(evbuf, "cann", queue_item->title);
  dmap_add_string(evbuf, "cana", queue_item->artist);
  dmap_add_string(evbuf, "canl", queue_item->album);
  dmap_add_string(evbuf, "cang", queue_item->genre);
  dmap_add_long(evbuf, "asai", songalbumid);

  dmap_add_int(evbuf, "cmmk", 1);
}

static void
dacp_playingtime(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  if ((status->status == PLAY_STOPPED) || !queue_item)
    return;

  if (queue_item->song_length)
    dmap_add_int(evbuf, "cant", queue_item->song_length - status->pos_ms); /* Remaining time in ms */
  else
    dmap_add_int(evbuf, "cant", 0); /* Unknown remaining time */

  dmap_add_int(evbuf, "cast", queue_item->song_length); /* Song length in ms */
}

static int
find_first_song_id(const char *query)
{
  struct db_media_file_info dbmfi;
  struct query_params qp;
  int id;
  int ret;

  id = 0;
  memset(&qp, 0, sizeof(struct query_params));

  /* We only want the id of the first song */
  qp.type = Q_ITEMS;
  qp.idx_type = I_FIRST;
  qp.sort = S_NONE;
  qp.offset = 0;
  qp.limit = 1;
  qp.filter = daap_query_parse_sql(query);
  if (!qp.filter)
    {
      DPRINTF(E_LOG, L_DACP, "Improper DAAP query!\n");

      return -1;
    }

  ret = db_query_start(&qp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not start query\n");

      goto no_query_start;
    }

  if (((ret = db_query_fetch_file(&qp, &dbmfi)) == 0) && (dbmfi.id))
    {
      ret = safe_atoi32(dbmfi.id, &id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DACP, "Invalid song id in query result!\n");

	  goto no_result;
	}

      DPRINTF(E_DBG, L_DACP, "Found index song (id %d)\n", id);
      ret = 1;
    }
  else
    {
      DPRINTF(E_LOG, L_DACP, "No song matches query (results %d): %s\n", qp.results, qp.filter);

      goto no_result;
    }

 no_result:
  db_query_end(&qp);

 no_query_start:
  if (qp.filter)
    free(qp.filter);

  if (ret == 1)
    return id;
  else
    return -1;
}

static int
dacp_queueitem_add(const char *query, const char *queuefilter, const char *sort, int quirk, int mode)
{
  struct media_file_info *mfi;
  struct query_params qp;
  int64_t albumid;
  int64_t artistid;
  int plid;
  int id;
  int ret;
  int len;
  char buf[1024];
  struct player_status status;

  if (query)
    {
      id = find_first_song_id(query);
      if (id < 0)
        return -1;
    }
  else
    id = 0;

  memset(&qp, 0, sizeof(struct query_params));

  qp.offset = 0;
  qp.limit = 0;
  qp.sort = S_NONE;
  qp.idx_type = I_NONE;

  if (quirk)
    {
      qp.sort = S_ALBUM;
      qp.type = Q_ITEMS;
      mfi = db_file_fetch_byid(id);
      if (!mfi)
	return -1;
      snprintf(buf, sizeof(buf), "f.songalbumid = %" PRIi64, mfi->songalbumid);
      free_mfi(mfi, 0);
      qp.filter = strdup(buf);
    }
  else if (queuefilter)
    {
      len = strlen(queuefilter);
      if ((len > 6) && (strncmp(queuefilter, "album:", 6) == 0))
	{
	  qp.type = Q_ITEMS;
	  ret = safe_atoi64(strchr(queuefilter, ':') + 1, &albumid);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_DACP, "Invalid album id in queuefilter: '%s'\n", queuefilter);

	      return -1;
	    }
	  snprintf(buf, sizeof(buf), "f.songalbumid = %" PRIi64, albumid);
	  qp.filter = strdup(buf);
	}
      else if ((len > 7) && (strncmp(queuefilter, "artist:", 7) == 0))
	{
	  qp.type = Q_ITEMS;
	  ret = safe_atoi64(strchr(queuefilter, ':') + 1, &artistid);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_DACP, "Invalid artist id in queuefilter: '%s'\n", queuefilter);

	      return -1;
	    }
	  snprintf(buf, sizeof(buf), "f.songartistid = %" PRIi64, artistid);
	  qp.filter = strdup(buf);
	}
      else if ((len > 9) && (strncmp(queuefilter, "playlist:", 9) == 0))
	{
	  qp.type = Q_PLITEMS;
	  ret = safe_atoi32(strchr(queuefilter, ':') + 1, &plid);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_DACP, "Invalid playlist id in queuefilter: '%s'\n", queuefilter);

	      return -1;
	    }
	  qp.id = plid;
	  qp.filter = strdup("1 = 1");
	}
      else if ((len > 6) && (strncmp(queuefilter, "genre:", 6) == 0))
	{
	  qp.type = Q_ITEMS;
	  ret = db_snprintf(buf, sizeof(buf), "f.genre = %Q", queuefilter + 6);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_DACP, "Invalid genre in queuefilter: '%s'\n", queuefilter);

	      return -1;
	    }
	  qp.filter = strdup(buf);
	}
      else
	{
	  DPRINTF(E_LOG, L_DACP, "Unknown queuefilter '%s', query is '%s'\n", queuefilter, query);

	  // If the queuefilter is unkown, ignore it and use the query parameter instead to build the sql query
	  id = 0;
	  qp.type = Q_ITEMS;
	  qp.filter = daap_query_parse_sql(query);
	}
    }
  else
    {
      id = 0;
      qp.type = Q_ITEMS;
      qp.filter = daap_query_parse_sql(query);
    }

  if (sort)
    {
      if (strcmp(sort, "name") == 0)
	qp.sort = S_NAME;
      else if (strcmp(sort, "album") == 0)
	qp.sort = S_ALBUM;
      else if (strcmp(sort, "artist") == 0)
	qp.sort = S_ARTIST;
    }

  player_get_status(&status);

  if (mode == 3)
    ret = db_queue_add_by_queryafteritemid(&qp, status.item_id);
  else
    ret = db_queue_add_by_query(&qp, status.shuffle, status.item_id);

  if (qp.filter)
    free(qp.filter);

  if (ret < 0)
    return -1;

  if (status.shuffle && mode != 1)
    return 0;

  return id;
}

static int
playqueuecontents_add_source(struct evbuffer *songlist, uint32_t source_id, int pos_in_queue, uint32_t plid)
{
  struct evbuffer *song;
  struct media_file_info *mfi;
  int ret;

  CHECK_NULL(L_DACP, song = evbuffer_new());
  CHECK_ERR(L_DACP, evbuffer_expand(song, 256));

  mfi = db_file_fetch_byid(source_id);
  if (!mfi)
    {
      DPRINTF(E_LOG, L_DACP, "Could not fetch file id %d\n", source_id);
      mfi = &dummy_mfi;
    }
  dmap_add_container(song, "ceQs", 16);
  dmap_add_raw_uint32(song, 1); /* Database */
  dmap_add_raw_uint32(song, plid);
  dmap_add_raw_uint32(song, 0); /* Should perhaps be playlist index? */
  dmap_add_raw_uint32(song, mfi->id);
  dmap_add_string(song, "ceQn", mfi->title);
  dmap_add_string(song, "ceQr", mfi->artist);
  dmap_add_string(song, "ceQa", mfi->album);
  dmap_add_string(song, "ceQg", mfi->genre);
  dmap_add_long(song, "asai", mfi->songalbumid);
  dmap_add_int(song, "cmmk", mfi->media_kind);
  dmap_add_int(song, "casa", 1); /* Unknown  */
  dmap_add_int(song, "astm", mfi->song_length);
  dmap_add_char(song, "casc", 1); /* Maybe an indication of extra data? */
  dmap_add_char(song, "caks", 6); /* Unknown */
  dmap_add_int(song, "ceQI", pos_in_queue);

  dmap_add_container(songlist, "mlit", evbuffer_get_length(song));

  ret = evbuffer_add_buffer(songlist, song);
  evbuffer_free(song);
  if (mfi != &dummy_mfi)
    free_mfi(mfi, 0);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not add song to songlist for playqueue-contents\n");
      return ret;
    }

  return 0;
}

static int
playqueuecontents_add_queue_item(struct evbuffer *songlist, struct db_queue_item *queue_item, int pos_in_queue, uint32_t plid)
{
  struct evbuffer *song;
  int ret;

  CHECK_NULL(L_DACP, song = evbuffer_new());
  CHECK_ERR(L_DACP, evbuffer_expand(song, 256));

  dmap_add_container(song, "ceQs", 16);
  dmap_add_raw_uint32(song, 1); /* Database */
  dmap_add_raw_uint32(song, plid);
  dmap_add_raw_uint32(song, 0); /* Should perhaps be playlist index? */
  dmap_add_raw_uint32(song, queue_item->file_id);
  dmap_add_string(song, "ceQn", queue_item->title);
  dmap_add_string(song, "ceQr", queue_item->artist);
  dmap_add_string(song, "ceQa", queue_item->album);
  dmap_add_string(song, "ceQg", queue_item->genre);
  dmap_add_long(song, "asai", queue_item->songalbumid);
  dmap_add_int(song, "cmmk", queue_item->media_kind);
  dmap_add_int(song, "casa", 1); /* Unknown  */
  dmap_add_int(song, "astm", queue_item->song_length);
  dmap_add_char(song, "casc", 1); /* Maybe an indication of extra data? */
  dmap_add_char(song, "caks", 6); /* Unknown */
  dmap_add_int(song, "ceQI", pos_in_queue);

  dmap_add_container(songlist, "mlit", evbuffer_get_length(song));

  ret = evbuffer_add_buffer(songlist, song);
  evbuffer_free(song);

  return ret;
}

static void
speaker_enum_cb(uint64_t id, const char *name, int relvol, int absvol, struct spk_flags flags, void *arg)
{
  struct evbuffer *evbuf;
  int len;

  evbuf = (struct evbuffer *)arg;

  len = 8 + strlen(name) + 28;
  if (flags.selected)
    len += 9;
  if (flags.has_password)
    len += 9;
  if (flags.has_video)
    len += 9;

  CHECK_ERR(L_DACP, evbuffer_expand(evbuf, 71 + len));

  dmap_add_container(evbuf, "mdcl", len); /* 8 + len */
  if (flags.selected)
    dmap_add_char(evbuf, "caia", 1);      /* 9 */
  if (flags.has_password)
    dmap_add_char(evbuf, "cahp", 1);      /* 9 */
  if (flags.has_video)
    dmap_add_char(evbuf, "caiv", 1);      /* 9 */
  dmap_add_string(evbuf, "minm", name);   /* 8 + len */
  dmap_add_long(evbuf, "msma", id);       /* 16 */

  dmap_add_int(evbuf, "cmvo", relvol);    /* 12 */
}

static struct dacp_request *
dacp_request_parse(struct evhttp_request *req, struct httpd_uri_parsed *uri_parsed)
{
  struct dacp_request *dreq;
  int i;
  int ret;

  CHECK_NULL(L_DACP, dreq = calloc(1, sizeof(struct dacp_request)));

  dreq->req        = req;
  dreq->uri_parsed = uri_parsed;
  dreq->query      = &(uri_parsed->ev_query);

  for (i = 0; dacp_handlers[i].handler; i++)
    {
      ret = regexec(&dacp_handlers[i].preg, uri_parsed->path, 0, NULL, 0);
      if (ret == 0)
        {
          dreq->handler = dacp_handlers[i].handler;
          break;
        }
    }

  if (!dreq->handler)
    {
      DPRINTF(E_LOG, L_DACP, "Unrecognized path '%s' in DACP request: '%s'\n", uri_parsed->path, uri_parsed->uri);
      goto error;
    }

  return dreq;

 error:
  free(dreq);

  return NULL;
}

static int
dacp_request_authorize(struct dacp_request *dreq)
{
  const char *param;
  int32_t id;
  int ret;

  if (cfg_getbool(cfg_getsec(cfg, "general"), "promiscuous_mode"))
    return 0;

  param = evhttp_find_header(dreq->query, "session-id");
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "No session-id specified in request\n");
      goto invalid;
    }

  ret = safe_atoi32(param, &id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Invalid session-id specified in request: '%s'\n", param);
      goto invalid;
    }

  if (!daap_session_is_valid(id))
    {
      DPRINTF(E_LOG, L_DACP, "Invalid session-id (%d) specified in request\n", id);
      goto invalid;
    }

  return 0;

 invalid:
  httpd_send_error(dreq->req, 403, "Forbidden");
  return -1;
}


/* ---------------------- UPDATE REQUESTS HANDLERS -------------------------- */

static int
make_playstatusupdate(struct evbuffer *evbuf)
{
  struct player_status status;
  struct db_queue_item *queue_item = NULL;
  struct evbuffer *psu;

  CHECK_NULL(L_DACP, psu = evbuffer_new());
  CHECK_ERR(L_DACP, evbuffer_expand(psu, 256));

  player_get_status(&status);
  if (status.status != PLAY_STOPPED)
    {
      queue_item = db_queue_fetch_byitemid(status.item_id);
      if (!queue_item)
	{
	  DPRINTF(E_LOG, L_DACP, "Could not fetch item id %d (file id %d)\n", status.item_id, status.id);

	  queue_item = &dummy_queue_item;
	}
    }

  dmap_add_int(psu, "mstt", 200);             /* 12 */

  dmap_add_int(psu, "cmsr", current_rev);     /* 12 */

  dmap_add_char(psu, "caps", status.status);  /*  9 */ /* play status, 2 = stopped, 3 = paused, 4 = playing */
  dmap_add_char(psu, "cash", status.shuffle); /*  9 */ /* shuffle, true/false */
  dmap_add_char(psu, "carp", status.repeat);  /*  9 */ /* repeat, 0 = off, 1 = repeat song, 2 = repeat (playlist) */
  dmap_add_char(psu, "cafs", 0);              /*  9 */ /* dacp.fullscreen */
  dmap_add_char(psu, "cavs", 0);              /*  9 */ /* dacp.visualizer */
  dmap_add_char(psu, "cavc", 1);              /*  9 */ /* volume controllable */
  dmap_add_int(psu, "caas", 2);               /* 12 */ /* available shuffle states */
  dmap_add_int(psu, "caar", 6);               /* 12 */ /* available repeat states */
  dmap_add_char(psu, "cafe", 0);              /*  9 */ /* dacp.fullscreenenabled */
  dmap_add_char(psu, "cave", 0);              /*  9 */ /* dacp.visualizerenabled */

  if (queue_item)
    {
      dacp_nowplaying(psu, &status, queue_item);

      dmap_add_int(psu, "casa", 1);           /* 12 */ /* unknown */
      dmap_add_int(psu, "astm", queue_item->song_length);
      dmap_add_char(psu, "casc", 1);          /* Maybe an indication of extra data? */
      dmap_add_char(psu, "caks", 6);          /* Unknown */

      dacp_playingtime(psu, &status, queue_item);

      if (queue_item != &dummy_queue_item)
	free_queue_item(queue_item, 0);
    }

  dmap_add_char(psu, "casu", 1);              /*  9 */ /* unknown */
  dmap_add_char(psu, "ceQu", 0);              /*  9 */ /* unknown */

  dmap_add_container(evbuf, "cmst", evbuffer_get_length(psu));    /* 8 + len */

  CHECK_ERR(L_DACP, evbuffer_add_buffer(evbuf, psu));

  evbuffer_free(psu);

  DPRINTF(E_DBG, L_DACP, "Replying to playstatusupdate with status %d and current_rev %d\n", status.status, current_rev);

  return 0;
}

static void
playstatusupdate_cb(int fd, short what, void *arg)
{
  struct dacp_update_request *ur;
  struct evbuffer *evbuf;
  struct evbuffer *update;
  struct evhttp_connection *evcon;
  uint8_t *buf;
  size_t len;
  int ret;

#ifdef HAVE_EVENTFD
  eventfd_t count;

  ret = eventfd_read(update_efd, &count);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not read playstatusupdate event counter: %s\n", strerror(errno));

      goto readd;
    }
#else
  int dummy;

  read(update_pipe[0], &dummy, sizeof(dummy));
#endif

  if (!update_requests)
    goto readd;

  CHECK_NULL(L_DACP, evbuf = evbuffer_new());
  CHECK_NULL(L_DACP, update = evbuffer_new());

  current_rev++;

  ret = make_playstatusupdate(update);
  if (ret < 0)
    goto out_free_update;

  len = evbuffer_get_length(update);

  for (ur = update_requests; update_requests; ur = update_requests)
    {
      update_requests = ur->next;

      evcon = evhttp_request_get_connection(ur->req);
      if (evcon)
	evhttp_connection_set_closecb(evcon, NULL, NULL);

      // Only copy buffer if we actually need to reuse it
      if (ur->next)
	{
	  buf = evbuffer_pullup(update, -1);
	  evbuffer_add(evbuf, buf, len);
	  httpd_send_reply(ur->req, HTTP_OK, "OK", evbuf, 0);
	}
      else
	httpd_send_reply(ur->req, HTTP_OK, "OK", update, 0);

      free(ur);
    }

 out_free_update:
  evbuffer_free(update);
  evbuffer_free(evbuf);
 readd:
  ret = event_add(updateev, NULL);
  if (ret < 0)
    DPRINTF(E_LOG, L_DACP, "Couldn't re-add event for playstatusupdate\n");
}

/* Thread: player */
static void
dacp_playstatus_update_handler(enum listener_event_type type)
{
  int ret;

  // Only send status update on player change events
  if (type != LISTENER_PLAYER)
    return;

#ifdef HAVE_EVENTFD
  ret = eventfd_write(update_efd, 1);
  if (ret < 0)
    DPRINTF(E_LOG, L_DACP, "Could not send status update event: %s\n", strerror(errno));
#else
  int dummy = 42;

  ret = write(update_pipe[1], &dummy, sizeof(dummy));
  if (ret != sizeof(dummy))
    DPRINTF(E_LOG, L_DACP, "Could not write to status update fd: %s\n", strerror(errno));
#endif
}

static void
update_fail_cb(struct evhttp_connection *evcon, void *arg)
{
  struct dacp_update_request *ur;
  struct dacp_update_request *p;
  struct evhttp_connection *evc;

  ur = (struct dacp_update_request *)arg;

  DPRINTF(E_DBG, L_DACP, "Update request: client closed connection\n");

  evc = evhttp_request_get_connection(ur->req);
  if (evc)
    evhttp_connection_set_closecb(evc, NULL, NULL);

  if (ur == update_requests)
    update_requests = ur->next;
  else
    {
      for (p = update_requests; p && (p->next != ur); p = p->next)
	;

      if (!p)
	{
	  DPRINTF(E_LOG, L_DACP, "WARNING: struct dacp_update_request not found in list; BUG!\n");
	  return;
	}

      p->next = ur->next;
    }

  free(ur);
}


/* --------------------- PROPERTIES GETTERS AND SETTERS --------------------- */

static void
dacp_propget_volume(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dmap_add_int(evbuf, "cmvo", status->volume);
}

static void
dacp_propget_volumecontrollable(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dmap_add_char(evbuf, "cavc", 1);
}

static void
dacp_propget_playerstate(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dmap_add_char(evbuf, "caps", status->status);
}

static void
dacp_propget_shufflestate(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dmap_add_char(evbuf, "cash", status->shuffle);
}

static void
dacp_propget_availableshufflestates(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dmap_add_int(evbuf, "caas", 2);
}

static void
dacp_propget_repeatstate(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dmap_add_char(evbuf, "carp", status->repeat);
}

static void
dacp_propget_availablerepeatstates(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dmap_add_int(evbuf, "caar", 6);
}

static void
dacp_propget_nowplaying(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dacp_nowplaying(evbuf, status, queue_item);
}

static void
dacp_propget_playingtime(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
  dacp_playingtime(evbuf, status, queue_item);
}

static void
dacp_propget_fullscreenenabled(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
	// TODO
}

static void
dacp_propget_fullscreen(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
	// TODO
}

static void
dacp_propget_visualizerenabled(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
	// TODO
}

static void
dacp_propget_visualizer(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
	// TODO
}

static void
dacp_propget_itms_songid(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
	// TODO
}

static void
dacp_propget_haschapterdata(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
	// TODO
}

static void
dacp_propget_mediakind(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
	// TODO
}

static void
dacp_propget_extendedmediakind(struct evbuffer *evbuf, struct player_status *status, struct db_queue_item *queue_item)
{
	// TODO
}

/* Properties setters */
static void
dacp_propset_volume(const char *value, struct evkeyvalq *query)
{
  const char *param;
  uint64_t id;
  int volume;
  int ret;

  ret = safe_atoi32(value, &volume);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "dmcp.volume argument doesn't convert to integer: %s\n", value);

      return;
    }

  param = evhttp_find_header(query, "speaker-id");
  if (param)
    {
      ret = safe_atou64(param, &id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DACP, "Invalid speaker ID in dmcp.volume request\n");

	  return;
	}

      player_volume_setrel_speaker(id, volume);
      return;
    }

  param = evhttp_find_header(query, "include-speaker-id");
  if (param)
    {
      ret = safe_atou64(param, &id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DACP, "Invalid speaker ID in dmcp.volume request\n");

	  return;
	}

      player_volume_setabs_speaker(id, volume);
      return;
    }

  player_volume_set(volume);
}

static void
seek_timer_cb(int fd, short what, void *arg)
{
  int ret;

  DPRINTF(E_DBG, L_DACP, "Seek timer expired, target %d ms\n", seek_target);

  ret = player_playback_seek(seek_target);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Player failed to seek to %d ms\n", seek_target);

      return;
    }

  ret = player_playback_start();
  if (ret < 0)
    DPRINTF(E_LOG, L_DACP, "Player returned an error for start after seek\n");
}

static void
dacp_propset_playingtime(const char *value, struct evkeyvalq *query)
{
  struct timeval tv;
  int ret;

  ret = safe_atoi32(value, &seek_target);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "dacp.playingtime argument doesn't convert to integer: %s\n", value);

      return;
    }

  evutil_timerclear(&tv);
  tv.tv_usec = 200 * 1000;
  evtimer_add(seek_timer, &tv);
}

static void
dacp_propset_shufflestate(const char *value, struct evkeyvalq *query)
{
  int enable;
  int ret;

  ret = safe_atoi32(value, &enable);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "dacp.shufflestate argument doesn't convert to integer: %s\n", value);

      return;
    }

  player_shuffle_set(enable);
}

static void
dacp_propset_repeatstate(const char *value, struct evkeyvalq *query)
{
  int mode;
  int ret;

  ret = safe_atoi32(value, &mode);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "dacp.repeatstate argument doesn't convert to integer: %s\n", value);

      return;
    }

  player_repeat_set(mode);
}

static void
dacp_propset_userrating(const char *value, struct evkeyvalq *query)
{
  struct media_file_info *mfi;
  const char *param;
  uint32_t itemid;
  uint32_t rating;
  int ret;

  ret = safe_atou32(value, &rating);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "dacp.userrating argument doesn't convert to integer: %s\n", value);

      return;
    }

  param = evhttp_find_header(query, "item-spec"); // Remote
  if (!param)
    param = evhttp_find_header(query, "song-spec"); // Retune

  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "Missing item-spec/song-spec parameter in dacp.userrating query\n");

      return;
    }

  param = strchr(param, ':');
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "Malformed item-spec/song-spec parameter in dacp.userrating query\n");

      return;
    }

  param++;

  if (strncmp(param, "0x", 2) == 0)
    ret = safe_hextou32(param, &itemid);
  else
    ret = safe_atou32(param, &itemid);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Couldn't convert item-spec/song-spec to an integer in dacp.userrating (%s)\n", param);

      return;
    }

  mfi = db_file_fetch_byid(itemid);

  /* If no mfi, it may be because we sent an invalid nowplaying itemid. In this
   * case request the real one from the player and default to that.
   */
  if (!mfi)
    {
      DPRINTF(E_WARN, L_DACP, "Invalid id %d for rating, defaulting to player id\n", itemid);

      ret = player_now_playing(&itemid);
      if ((ret < 0) || !(mfi = db_file_fetch_byid(itemid)))
	{
	  DPRINTF(E_WARN, L_DACP, "Could not find an id for rating\n");

	  return;
	}
    }

  mfi->rating = rating;

  /* We're not touching any string field in mfi, so it's safe to
   * skip unicode_fixup_mfi() before the update
   */
  db_file_update(mfi);

  free_mfi(mfi, 0);
}


/* --------------------------- REPLY HANDLERS ------------------------------- */

static void
dacp_reply_ctrlint(struct evbuffer *reply, struct dacp_request *dreq)
{
  /* /ctrl-int */
  CHECK_ERR(L_DACP, evbuffer_expand(reply, 256));

  /* If tags are added or removed container sizes should be adjusted too */
  dmap_add_container(reply, "caci", 194); /*  8, unknown dacp container - size of content */
  dmap_add_int(reply, "mstt", 200);       /* 12, dmap.status */
  dmap_add_char(reply, "muty", 0);        /*  9, dmap.updatetype */
  dmap_add_int(reply, "mtco", 1);         /* 12, dmap.specifiedtotalcount */
  dmap_add_int(reply, "mrco", 1);         /* 12, dmap.returnedcount */
  dmap_add_container(reply, "mlcl", 141); /*  8, dmap.listing - size of content */
  dmap_add_container(reply, "mlit", 133); /*  8, dmap.listingitem - size of content */
  dmap_add_int(reply, "miid", 1);         /* 12, dmap.itemid - database ID */
  dmap_add_char(reply, "cmik", 1);        /*  9, unknown */

  dmap_add_int(reply, "cmpr", (2 << 16 | 2)); /* 12, dmcp.protocolversion */
  dmap_add_int(reply, "capr", (2 << 16 | 5)); /* 12, dacp.protocolversion */

  dmap_add_char(reply, "cmsp", 1);        /*  9, unknown */
  dmap_add_char(reply, "aeFR", 0x64);     /*  9, unknown */
  dmap_add_char(reply, "cmsv", 1);        /*  9, unknown */
  dmap_add_char(reply, "cass", 1);        /*  9, unknown */
  dmap_add_char(reply, "caov", 1);        /*  9, unknown */
  dmap_add_char(reply, "casu", 1);        /*  9, unknown */
  dmap_add_char(reply, "ceSG", 1);        /*  9, unknown */
  dmap_add_char(reply, "cmrl", 1);        /*  9, unknown */
  dmap_add_long(reply, "ceSX", (1 << 1 | 1));  /* 16, unknown dacp - lowest bit announces support for playqueue-contents/-edit */

  httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, 0);
}

static void
dacp_reply_cue_play(struct evbuffer *reply, struct dacp_request *dreq)
{
  struct player_status status;
  const char *sort;
  const char *cuequery;
  const char *param;
  uint32_t pos;
  int clear;
  struct db_queue_item *queue_item = NULL;
  struct player_history *history;
  int ret;

  /* /cue?command=play&query=...&sort=...&index=N */

  param = evhttp_find_header(dreq->query, "clear-first");
  if (param)
    {
      ret = safe_atoi32(param, &clear);
      if (ret < 0)
	DPRINTF(E_LOG, L_DACP, "Invalid clear-first value in cue request\n");
      else if (clear)
	{
	  player_playback_stop();

	  db_queue_clear(0);
	}
    }

  player_get_status(&status);

  cuequery = evhttp_find_header(dreq->query, "query");
  if (cuequery)
    {
      sort = evhttp_find_header(dreq->query, "sort");

      ret = dacp_queueitem_add(cuequery, NULL, sort, 0, 0);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DACP, "Could not build song queue\n");

	  dmap_send_error(dreq->req, "cacr", "Could not build song queue");
	  return;
	}
    }
  else if (status.status != PLAY_STOPPED)
    {
      player_playback_stop();
    }

  param = evhttp_find_header(dreq->query, "dacp.shufflestate");
  if (param)
    dacp_propset_shufflestate(param, NULL);

  pos = 0;
  param = evhttp_find_header(dreq->query, "index");
  if (param)
    {
      ret = safe_atou32(param, &pos);
      if (ret < 0)
	DPRINTF(E_LOG, L_DACP, "Invalid index (%s) in cue request\n", param);
    }

  /* If selection was from Up Next queue or history queue (command will be playnow), then index is relative */
  if ((param = evhttp_find_header(dreq->query, "command")) && (strcmp(param, "playnow") == 0))
    {
      /* If mode parameter is -1 or 1, the index is relative to the history queue, otherwise to the Up Next queue */
      param = evhttp_find_header(dreq->query, "mode");
      if (param && ((strcmp(param, "-1") == 0) || (strcmp(param, "1") == 0)))
	{
	  /* Play from history queue */
	  history = player_history_get();
	  if (history->count > pos)
	    {
	      pos = (history->start_index + history->count - pos - 1) % MAX_HISTORY_COUNT;

	      queue_item = db_queue_fetch_byitemid(history->item_id[pos]);
	      if (!queue_item)
		{
		  DPRINTF(E_LOG, L_DACP, "Could not start playback from history\n");

		  dmap_send_error(dreq->req, "cacr", "Playback failed to start");
		  return;
		}
	    }
	  else
	    {
	      DPRINTF(E_LOG, L_DACP, "Could not start playback from history\n");

	      dmap_send_error(dreq->req, "cacr", "Playback failed to start");
	      return;
	    }
	}
      else
	{
	  /* Play from Up Next queue */
	  if (status.status == PLAY_STOPPED && pos > 0)
	    pos--;

	  queue_item = db_queue_fetch_byposrelativetoitem(pos, status.item_id, status.shuffle);
	  if (!queue_item)
	    {
	      DPRINTF(E_LOG, L_DACP, "Could not fetch item from queue: pos=%d, now playing=%d\n", pos, status.item_id);

	      dmap_send_error(dreq->req, "cacr", "Playback failed to start");
	      return;
	    }
	}
    }
  else
    {
      queue_item = db_queue_fetch_bypos(pos, status.shuffle);
      if (!queue_item)
	{
	  DPRINTF(E_LOG, L_DACP, "Could not fetch item from queue: pos=%d\n", pos);

	  dmap_send_error(dreq->req, "cacr", "Playback failed to start");
	  return;
	}
    }

  ret = player_playback_start_byitem(queue_item);
  free_queue_item(queue_item, 0);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not start playback\n");

      dmap_send_error(dreq->req, "cacr", "Playback failed to start");
      return;
    }

  player_get_status(&status);

  CHECK_ERR(L_DACP, evbuffer_expand(reply, 64));

  dmap_add_container(reply, "cacr", 24); /* 8 + len */
  dmap_add_int(reply, "mstt", 200);      /* 12 */
  dmap_add_int(reply, "miid", status.id);/* 12 */

  httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, 0);
}

static void
dacp_reply_cue_clear(struct evbuffer *reply, struct dacp_request *dreq)
{
  /* /cue?command=clear */

  player_playback_stop();

  db_queue_clear(0);

  CHECK_ERR(L_DACP, evbuffer_expand(reply, 64));

  dmap_add_container(reply, "cacr", 24); /* 8 + len */
  dmap_add_int(reply, "mstt", 200);      /* 12 */
  dmap_add_int(reply, "miid", 0);        /* 12 */

  httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, 0);
}

static void
dacp_reply_cue(struct evbuffer *reply, struct dacp_request *dreq)
{
  const char *param;
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  param = evhttp_find_header(dreq->query, "command");
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "No command in cue request\n");

      dmap_send_error(dreq->req, "cacr", "No command in cue request");
      return;
    }

  if (strcmp(param, "clear") == 0)
    dacp_reply_cue_clear(reply, dreq);
  else if (strcmp(param, "play") == 0)
    dacp_reply_cue_play(reply, dreq);
  else
    {
      DPRINTF(E_LOG, L_DACP, "Unknown cue command %s\n", param);

      dmap_send_error(dreq->req, "cacr", "Unknown command in cue request");
      return;
    }
}

static void
dacp_reply_playspec(struct evbuffer *reply, struct dacp_request *dreq)
{
  struct player_status status;
  const char *param;
  const char *shuffle;
  uint32_t plid;
  uint32_t id;
  struct db_queue_item *queue_item = NULL;
  int ret;

  /* /ctrl-int/1/playspec?database-spec='dmap.persistentid:0x1'&container-spec='dmap.persistentid:0x5'&container-item-spec='dmap.containeritemid:0x9'
   * or (Apple Remote when playing a Podcast)
   * /ctrl-int/1/playspec?database-spec='dmap.persistentid:0x1'&container-spec='dmap.persistentid:0x5'&item-spec='dmap.itemid:0x9'
   * With our DAAP implementation, container-spec is the playlist ID and container-item-spec/item-spec is the song ID
   */

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  /* Check for shuffle */
  shuffle = evhttp_find_header(dreq->query, "dacp.shufflestate");

  /* Playlist ID */
  param = evhttp_find_header(dreq->query, "container-spec");
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "No container-spec in playspec request\n");

      goto out_fail;
    }

  param = strchr(param, ':');
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "Malformed container-spec parameter in playspec request\n");

      goto out_fail;
    }
  param++;

  if (strncmp(param, "0x", 2) == 0)
    ret = safe_hextou32(param, &plid);
  else
    ret = safe_atou32(param, &plid);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Couldn't convert container-spec to an integer in playspec (%s)\n", param);

      goto out_fail;
    }

  if (!shuffle)
    {
      /* Start song ID */
      if ((param = evhttp_find_header(dreq->query, "item-spec")))
	plid = 0; // This is a podcast/audiobook - just play a single item, not a playlist
      else if (!(param = evhttp_find_header(dreq->query, "container-item-spec")))
	{
	  DPRINTF(E_LOG, L_DACP, "No container-item-spec/item-spec in playspec request\n");

	  goto out_fail;
	}

      param = strchr(param, ':');
      if (!param)
	{
	  DPRINTF(E_LOG, L_DACP, "Malformed container-item-spec/item-spec parameter in playspec request\n");

	  goto out_fail;
	}
      param++;

      if (strncmp(param, "0x", 2) == 0)
	ret = safe_hextou32(param, &id);
      else
	ret = safe_atou32(param, &id);

      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DACP, "Couldn't convert container-item-spec/item-spec to an integer in playspec (%s)\n", param);

	  goto out_fail;
	}
    }
  else
    id = 0;

  DPRINTF(E_DBG, L_DACP, "Playspec request for playlist %d, start song id %d%s\n", plid, id, (shuffle) ? ", shuffle" : "");

  player_get_status(&status);

  if (status.status != PLAY_STOPPED)
    player_playback_stop();

  db_queue_clear(0);

  if (plid > 0)
    ret = db_queue_add_by_playlistid(plid, status.shuffle, status.item_id);
  else if (id > 0)
    ret = db_queue_add_by_fileid(id, status.shuffle, status.item_id);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not build song queue from playlist %d\n", plid);

      goto out_fail;
    }

  player_queue_plid(plid);

  if (shuffle)
    dacp_propset_shufflestate(shuffle, NULL);

  if (id > 0)
    queue_item = db_queue_fetch_byfileid(id);

  if (queue_item)
    {
      ret = player_playback_start_byitem(queue_item);
      free_queue_item(queue_item, 0);
    }
  else
    ret = player_playback_start();

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not start playback\n");

      goto out_fail;
    }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
  return;

 out_fail:
  httpd_send_error(dreq->req, 500, "Internal Server Error");
}

static void
dacp_reply_pause(struct evbuffer *reply, struct dacp_request *dreq)
{
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  player_playback_pause();

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_playpause(struct evbuffer *reply, struct dacp_request *dreq)
{
  struct player_status status;
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  player_get_status(&status);
  if (status.status == PLAY_PLAYING)
    {
      player_playback_pause();
    }
  else
    {
      ret = player_playback_start();
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DACP, "Player returned an error for start after pause\n");

	  httpd_send_error(dreq->req, 500, "Internal Server Error");
	  return;
        }
    }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_nextitem(struct evbuffer *reply, struct dacp_request *dreq)
{
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  ret = player_playback_next();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Player returned an error for nextitem\n");

      httpd_send_error(dreq->req, 500, "Internal Server Error");
      return;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Player returned an error for start after nextitem\n");

      httpd_send_error(dreq->req, 500, "Internal Server Error");
      return;
    }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_previtem(struct evbuffer *reply, struct dacp_request *dreq)
{
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  ret = player_playback_prev();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Player returned an error for previtem\n");

      httpd_send_error(dreq->req, 500, "Internal Server Error");
      return;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Player returned an error for start after previtem\n");

      httpd_send_error(dreq->req, 500, "Internal Server Error");
      return;
    }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_beginff(struct evbuffer *reply, struct dacp_request *dreq)
{
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  /* TODO */

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_beginrew(struct evbuffer *reply, struct dacp_request *dreq)
{
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  /* TODO */

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_playresume(struct evbuffer *reply, struct dacp_request *dreq)
{
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  /* TODO */

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_playqueuecontents(struct evbuffer *reply, struct dacp_request *dreq)
{
  struct evbuffer *songlist;
  struct evbuffer *playlists;
  struct player_status status;
  struct player_history *history;
  const char *param;
  size_t songlist_length;
  size_t playlist_length;
  int span;
  int count;
  int ret;
  int start_index;
  struct query_params qp;
  struct db_queue_item queue_item;

  /* /ctrl-int/1/playqueue-contents?span=50&session-id=... */

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  DPRINTF(E_DBG, L_DACP, "Fetching playqueue contents\n");

  span = 50; /* Default */
  param = evhttp_find_header(dreq->query, "span");
  if (param)
    {
      ret = safe_atoi32(param, &span);
      if (ret < 0)
	DPRINTF(E_LOG, L_DACP, "Invalid span value in playqueue-contents request\n");
    }

  CHECK_NULL(L_DACP, songlist = evbuffer_new());
  CHECK_ERR(L_DACP, evbuffer_expand(reply, 128));

  player_get_status(&status);

  count = 0; // count of songs in songlist

  /*
   * If the span parameter is negativ make song list for Previously Played,
   * otherwise make song list for Up Next and begin with first song after playlist position.
   */
  if (span < 0)
    {
      history = player_history_get();
      if (abs(span) > history->count)
	{
	  start_index = history->start_index;
	}
      else
	{
	  start_index = (history->start_index + history->count - abs(span)) % MAX_HISTORY_COUNT;
	}

      while ((count < history->count) && (count < abs(span)))
	{
	  ret = playqueuecontents_add_source(songlist, history->id[(start_index + count) % MAX_HISTORY_COUNT], (count + 1), status.plid);
	  if (ret < 0)
	    goto error;

	  count++;
	}
    }
  else
    {
      memset(&qp, 0, sizeof(struct query_params));

      if (status.shuffle)
	qp.sort = S_SHUFFLE_POS;

      ret = db_queue_enum_start(&qp);
      if (ret < 0)
	goto error;

      //FIXME [queue] Check count value
      while ((db_queue_enum_fetch(&qp, &queue_item) == 0) && (queue_item.id > 0))
	{
	  if (status.item_id == 0 || status.item_id == queue_item.id)
	    {
	      count = 1;
	    }
	  else if (count > 0)
	    {
	      ret = playqueuecontents_add_queue_item(songlist, &queue_item, count, status.plid);
	      if (ret < 0)
		goto error;

	      count++;
	    }
	}

      db_queue_enum_end(&qp);
    }

  /* Playlists are hist, curr and main. */
  CHECK_NULL(L_DACP, playlists = evbuffer_new());
  CHECK_ERR(L_DACP, evbuffer_expand(playlists, 256));

  dmap_add_container(playlists, "mlit", 61);               /*  8 */
  dmap_add_string(playlists, "ceQk", "hist");              /* 12 */
  dmap_add_int(playlists, "ceQi", -200);                   /* 12 */
  dmap_add_int(playlists, "ceQm", 200);                    /* 12 */
  dmap_add_string(playlists, "ceQl", "Previously Played"); /* 25 = 8 + 17 */

  if (count > 0)
    {
      dmap_add_container(playlists, "mlit", 36);             /*  8 */
      dmap_add_string(playlists, "ceQk", "curr");            /* 12 */
      dmap_add_int(playlists, "ceQi", 0);                    /* 12 */
      dmap_add_int(playlists, "ceQm", 1);                    /* 12 */

      dmap_add_container(playlists, "mlit", 69);
      dmap_add_string(playlists, "ceQk", "main");            /* 12 */
      dmap_add_int(playlists, "ceQi", 1);                    /* 12 */
      dmap_add_int(playlists, "ceQm", count);                /* 12 */
      dmap_add_string(playlists, "ceQl", "Up Next");         /* 15 = 8 + 7 */
      dmap_add_string(playlists, "ceQh", "from Music");      /* 18 = 8 + 10 */

      songlist_length = evbuffer_get_length(songlist);
    }
  else
    songlist_length = 0;

  /* Final construction of reply */
  playlist_length = evbuffer_get_length(playlists);
  dmap_add_container(reply, "ceQR", 79 + playlist_length + songlist_length); /* size of entire container */
  dmap_add_int(reply, "mstt", 200);                                          /* 12, dmap.status */
  dmap_add_int(reply, "mtco", abs(span));                                    /* 12 */
  dmap_add_int(reply, "mrco", count);                                        /* 12 */
  dmap_add_char(reply, "ceQu", 0);                                           /*  9 */
  dmap_add_container(reply, "mlcl", 8 + playlist_length + songlist_length);  /*  8 */
  dmap_add_container(reply, "ceQS", playlist_length);                        /*  8 */

  CHECK_ERR(L_DACP, evbuffer_add_buffer(reply, playlists));
  CHECK_ERR(L_DACP, evbuffer_add_buffer(reply, songlist));

  evbuffer_free(playlists);
  evbuffer_free(songlist);

  dmap_add_char(reply, "apsm", status.shuffle); /*  9, daap.playlistshufflemode - not part of mlcl container */
  dmap_add_char(reply, "aprm", status.repeat);  /*  9, daap.playlistrepeatmode  - not part of mlcl container */

  httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, 0);

  return;

 error:
  DPRINTF(E_LOG, L_DACP, "Database error in dacp_reply_playqueuecontents\n");

  evbuffer_free(songlist);
  dmap_send_error(dreq->req, "ceQR", "Database error");
}

static void
dacp_reply_playqueueedit_clear(struct evbuffer *reply, struct dacp_request *dreq)
{
  const char *param;
  struct player_status status;

  param = evhttp_find_header(dreq->query, "mode");

  /*
   * The mode parameter contains the playlist to be cleared.
   * If mode=0x68697374 (hex representation of the ascii string "hist") clear the history,
   * otherwise the current playlist.
   */
  if (strcmp(param,"0x68697374") == 0)
    player_queue_clear_history();
  else
    {
      player_get_status(&status);
      db_queue_clear(status.item_id);
    }

  dmap_add_container(reply, "cacr", 24); /* 8 + len */
  dmap_add_int(reply, "mstt", 200);      /* 12 */
  dmap_add_int(reply, "miid", 0);        /* 12 */

  httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, 0);
}

static void
dacp_reply_playqueueedit_add(struct evbuffer *reply, struct dacp_request *dreq)
{
  //?command=add&query='dmap.itemid:156'&sort=album&mode=3&session-id=100
  // -> mode=3: add to playqueue position 0 (play next)
  //?command=add&query='dmap.itemid:158'&sort=album&mode=0&session-id=100
  // -> mode=0: add to end of playqueue
  //?command=add&query='dmap.itemid:306'&queuefilter=album:6525753023700533274&sort=album&mode=1&session-id=100
  // -> mode 1: stop playblack, clear playqueue, add songs to playqueue
  //?command=add&query='dmap.itemid:2'&query-modifier=containers&sort=name&mode=2&session-id=100
  // -> mode 2: stop playblack, clear playqueue, add shuffled songs from playlist=itemid to playqueue

  const char *editquery;
  const char *queuefilter;
  const char *querymodifier;
  const char *sort;
  const char *param;
  char modifiedquery[32];
  int mode;
  int plid;
  int ret;
  int quirkyquery;
  struct db_queue_item *queue_item;
  struct player_status status;

  mode = 1;

  param = evhttp_find_header(dreq->query, "mode");
  if (param)
    {
      ret = safe_atoi32(param, &mode);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DACP, "Invalid mode value in playqueue-edit request\n");

	  dmap_send_error(dreq->req, "cacr", "Invalid request");
	  return;
	}
    }

  if ((mode == 1) || (mode == 2))
    {
      player_playback_stop();
      db_queue_clear(0);
    }

  if (mode == 2)
    player_shuffle_set(1);

  editquery = evhttp_find_header(dreq->query, "query");
  if (!editquery)
    {
      DPRINTF(E_LOG, L_DACP, "Could not add song queue, DACP query missing\n");

      dmap_send_error(dreq->req, "cacr", "Invalid request");
      return;
    }

  sort = evhttp_find_header(dreq->query, "sort");

  // if sort param is missing and an album or artist is added to the queue, set sort to "album"
  if (!sort && (strstr(editquery, "daap.songalbumid:") || strstr(editquery, "daap.songartistid:")))
    {
      sort = "album";
    }

  // only use queryfilter if mode is not equal 0 (add to up next), 3 (play next) or 5 (add to up next)
  queuefilter = (mode == 0 || mode == 3 || mode == 5) ? NULL : evhttp_find_header(dreq->query, "queuefilter");

  querymodifier = evhttp_find_header(dreq->query, "query-modifier");
  if (!querymodifier || (strcmp(querymodifier, "containers") != 0))
    {
      quirkyquery = (mode == 1) && strstr(editquery, "dmap.itemid:") && ((!queuefilter) || strstr(queuefilter, "(null)"));
      ret = dacp_queueitem_add(editquery, queuefilter, sort, quirkyquery, mode);
    }
  else
    {
      // Modify the query: Take the id from the editquery and use it as a queuefilter playlist id
      ret = safe_atoi32(strchr(editquery, ':') + 1, &plid);
      if (ret < 0)
        {
	  DPRINTF(E_LOG, L_DACP, "Invalid playlist id in request: %s\n", editquery);

	  dmap_send_error(dreq->req, "cacr", "Invalid request");
	  return;
	}

      snprintf(modifiedquery, sizeof(modifiedquery), "playlist:%d", plid);
      ret = dacp_queueitem_add(NULL, modifiedquery, sort, 0, mode);
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not build song queue\n");

      dmap_send_error(dreq->req, "cacr", "Invalid request");
      return;
    }

  if (ret > 0)
    queue_item = db_queue_fetch_byfileid(ret);
  else
    queue_item = NULL;

  if (queue_item)
    {
      player_get_status(&status);

      if (status.shuffle)
	{
	  DPRINTF(E_DBG, L_DACP, "Start shuffle queue with item %d\n", queue_item->id);
	  db_queue_move_byitemid(queue_item->id, 0, status.shuffle);
	}

      DPRINTF(E_DBG, L_DACP, "Song queue built, starting playback at index %d\n", queue_item->pos);
      ret = player_playback_start_byitem(queue_item);
      free_queue_item(queue_item, 0);
    }
  else
    {
      DPRINTF(E_DBG, L_DACP, "Song queue built, starting playback\n");
      ret = player_playback_start();
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not start playback\n");

      dmap_send_error(dreq->req, "cacr", "Playback failed to start");
      return;
    }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_playqueueedit_move(struct evbuffer *reply, struct dacp_request *dreq)
{
  /*
   * Handles the move command.
   * Exampe request:
   * playqueue-edit?command=move&edit-params='edit-param.move-pair:3,0'&session-id=100
   *
   * The 'edit-param.move-pair' param contains the index of the song in the playqueue to be moved (index 3 in the example)
   * and the index of the song after which it should be inserted (index 0 in the exampe, the now playing song).
   */
  struct player_status status;
  int ret;
  const char *param;
  int src;
  int dst;

  param = evhttp_find_header(dreq->query, "edit-params");
  if (param)
  {
    ret = safe_atoi32(strchr(param, ':') + 1, &src);
    if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Invalid edit-params move-from value in playqueue-edit request\n");

      dmap_send_error(dreq->req, "cacr", "Invalid request");
      return;
    }

    ret = safe_atoi32(strchr(param, ',') + 1, &dst);
    if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Invalid edit-params move-to value in playqueue-edit request\n");

      dmap_send_error(dreq->req, "cacr", "Invalid request");
      return;
    }

    player_get_status(&status);
    db_queue_move_byposrelativetoitem(src, dst, status.item_id, status.shuffle);
  }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_playqueueedit_remove(struct evbuffer *reply, struct dacp_request *dreq)
{
  /*
   * Handles the remove command.
   * Exampe request (removes song at position 1 in the playqueue):
   * ?command=remove&items=1&session-id=100
   */
  struct player_status status;
  const char *param;
  int item_index;
  int ret;

  param = evhttp_find_header(dreq->query, "items");
  if (param)
  {
    ret = safe_atoi32(param, &item_index);
    if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Invalid edit-params remove item value in playqueue-edit request\n");

      dmap_send_error(dreq->req, "cacr", "Invalid request");
      return;
    }

    player_get_status(&status);

    db_queue_delete_byposrelativetoitem(item_index, status.item_id, status.shuffle);
  }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_playqueueedit(struct evbuffer *reply, struct dacp_request *dreq)
{
  const char *param;
  int ret;

  /*  Variations of /ctrl-int/1/playqueue-edit and expected behaviour
      User selected play (album or artist tab):
	?command=add&query='...'&sort=album&mode=1&session-id=...
	-> clear queue, play query results
      User selected play (playlist):
	?command=add&query='dmap.itemid:...'&query-modifier=containers&mode=1&session-id=...
	-> clear queue, play playlist with the id specified by itemid
      User selected track (album tab):
	?command=add&query='dmap.itemid:...'&queuefilter=album:...&sort=album&mode=1&session-id=...
	-> clear queue, play itemid and the rest of album
      User selected track (artist tab):
	?command=add&query='dmap.itemid:...'&queuefilter=artist:...&sort=album&mode=1&session-id=...
	-> clear queue, play itemid and the rest of artist tracks
      User selected track (song tab):
	?command=add&query='dmap.itemid:...'&queuefilter=playlist:...&sort=name&mode=1&session-id=...
	-> clear queue, play itemid and the rest of playlist
      User selected track (playlist tab):
	?command=add&query='dmap.containeritemid:...'&queuefilter=playlist:...&sort=physical&mode=1&session-id=...
	-> clear queue, play containeritemid and the rest of playlist
      User selected shuffle (artist tab):
	?command=add&query='...'&sort=album&mode=2&session-id=...
	-> clear queue, play shuffled query results
      User selected add item to queue:
	?command=add&query='...'&sort=album&mode=0&session-id=...
	-> add query results to queue
      User selected play next song (album tab)
	?command=add&query='daap.songalbumid:...'&sort=album&mode=3&session-id=...
	-> replace queue from after current song with query results
      User selected track in queue:
	?command=playnow&index=...&session-id=...
	-> play index

      And the quirky query from Remote - no sort and no queuefilter
      User selected track (Audiobooks):
	?command=add&query='dmap.itemid:...'&mode=1&session-id=...
	-> clear queue, play itemid and the rest of album tracks

	?command=move&edit-params='edit-param.move-pair:3,0'&session-id=100
	-> move song from playqueue position 3 to be played after song at position 0
	?command=remove&items=1&session-id=100
  -> remove song on position 1 from the playqueue
   */

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  param = evhttp_find_header(dreq->query, "command");
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "No command in playqueue-edit request\n");

      dmap_send_error(dreq->req, "cmst", "Invalid request");
      return;
    }

  if (strcmp(param, "clear") == 0)
    dacp_reply_playqueueedit_clear(reply, dreq);
  else if (strcmp(param, "playnow") == 0)
    dacp_reply_cue_play(reply, dreq);
  else if (strcmp(param, "add") == 0)
    dacp_reply_playqueueedit_add(reply, dreq);
  else if (strcmp(param, "move") == 0)
    dacp_reply_playqueueedit_move(reply, dreq);
  else if (strcmp(param, "remove") == 0)
    dacp_reply_playqueueedit_remove(reply, dreq);
  else
    {
      DPRINTF(E_LOG, L_DACP, "Unknown playqueue-edit command %s\n", param);

      dmap_send_error(dreq->req, "cmst", "Invalid request");
      return;
    }
}

static void
dacp_reply_playstatusupdate(struct evbuffer *reply, struct dacp_request *dreq)
{
  struct dacp_update_request *ur;
  struct evhttp_connection *evcon;
  const char *param;
  int reqd_rev;
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  param = evhttp_find_header(dreq->query, "revision-number");
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "Missing revision-number in update request\n");

      dmap_send_error(dreq->req, "cmst", "Invalid request");
      return;
    }

  ret = safe_atoi32(param, &reqd_rev);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Parameter revision-number not an integer\n");

      dmap_send_error(dreq->req, "cmst", "Invalid request");
      return;
    }

  if ((reqd_rev == 0) || (reqd_rev == 1))
    {
      ret = make_playstatusupdate(reply);
      if (ret < 0)
	httpd_send_error(dreq->req, 500, "Internal Server Error");
      else
	httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, 0);

      return;
    }

  /* Else, just let the request hang until we have changes to push back */
  ur = calloc(1, sizeof(struct dacp_update_request));
  if (!ur)
    {
      DPRINTF(E_LOG, L_DACP, "Out of memory for update request\n");

      dmap_send_error(dreq->req, "cmst", "Out of memory");
      return;
    }

  ur->req = dreq->req;

  ur->next = update_requests;
  update_requests = ur;

  /* If the connection fails before we have an update to push out
   * to the client, we need to know.
   */
  evcon = evhttp_request_get_connection(dreq->req);
  if (evcon)
    evhttp_connection_set_closecb(evcon, update_fail_cb, ur);
}

static void
dacp_reply_nowplayingartwork(struct evbuffer *reply, struct dacp_request *dreq)
{
  char clen[32];
  struct evkeyvalq *headers;
  const char *param;
  char *ctype;
  size_t len;
  uint32_t id;
  int max_w;
  int max_h;
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  param = evhttp_find_header(dreq->query, "mw");
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "Request for artwork without mw parameter\n");

      httpd_send_error(dreq->req, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  ret = safe_atoi32(param, &max_w);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not convert mw parameter to integer\n");

      httpd_send_error(dreq->req, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  param = evhttp_find_header(dreq->query, "mh");
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "Request for artwork without mh parameter\n");

      httpd_send_error(dreq->req, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  ret = safe_atoi32(param, &max_h);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not convert mh parameter to integer\n");

      httpd_send_error(dreq->req, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  ret = player_now_playing(&id);
  if (ret < 0)
    goto no_artwork;

  ret = artwork_get_item(reply, id, max_w, max_h);
  len = evbuffer_get_length(reply);

  switch (ret)
    {
      case ART_FMT_PNG:
	ctype = "image/png";
	break;

      case ART_FMT_JPEG:
	ctype = "image/jpeg";
	break;

      default:
	if (len > 0)
	  evbuffer_drain(reply, len);

	goto no_artwork;
    }

  headers = evhttp_request_get_output_headers(dreq->req);
  evhttp_remove_header(headers, "Content-Type");
  evhttp_add_header(headers, "Content-Type", ctype);
  snprintf(clen, sizeof(clen), "%ld", (long)len);
  evhttp_add_header(headers, "Content-Length", clen);

  httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, HTTPD_SEND_NO_GZIP);
  return;

 no_artwork:
  httpd_send_error(dreq->req, HTTP_NOTFOUND, "Not Found");
}

static void
dacp_reply_getproperty(struct evbuffer *reply, struct dacp_request *dreq)
{
  struct player_status status;
  const struct dacp_prop_map *dpm;
  struct db_queue_item *queue_item = NULL;
  struct evbuffer *proplist;
  const char *param;
  char *ptr;
  char *prop;
  char *propstr;
  size_t len;
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  param = evhttp_find_header(dreq->query, "properties");
  if (!param)
    {
      DPRINTF(E_WARN, L_DACP, "Invalid DACP getproperty request, no properties\n");

      dmap_send_error(dreq->req, "cmgt", "Invalid request");
      return;
    }

  propstr = strdup(param);
  if (!propstr)
    {
      DPRINTF(E_LOG, L_DACP, "Could not duplicate properties parameter; out of memory\n");

      dmap_send_error(dreq->req, "cmgt", "Out of memory");
      return;
    }

  proplist = evbuffer_new();
  if (!proplist)
    {
      DPRINTF(E_LOG, L_DACP, "Could not allocate evbuffer for properties list\n");

      dmap_send_error(dreq->req, "cmgt", "Out of memory");
      goto out_free_propstr;
    }

  player_get_status(&status);

  if (status.status != PLAY_STOPPED)
    {
      queue_item = db_queue_fetch_byitemid(status.item_id);
      if (!queue_item)
	{
	  DPRINTF(E_LOG, L_DACP, "Could not fetch queue_item for item-id %d\n", status.item_id);

	  dmap_send_error(dreq->req, "cmgt", "Server error");
	  goto out_free_proplist;
	}
    }

  prop = strtok_r(propstr, ",", &ptr);
  while (prop)
    {
      dpm = dacp_find_prop(prop, strlen(prop));
      if (dpm)
	{
	  if (dpm->propget)
	    dpm->propget(proplist, &status, queue_item);
	  else
	    DPRINTF(E_WARN, L_DACP, "No getter method for DACP property %s\n", prop);
	}
      else
	DPRINTF(E_LOG, L_DACP, "Could not find requested property '%s'\n", prop);

      prop = strtok_r(NULL, ",", &ptr);
    }

  free(propstr);

  if (queue_item)
    free_queue_item(queue_item, 0);

  len = evbuffer_get_length(proplist);
  dmap_add_container(reply, "cmgt", 12 + len);
  dmap_add_int(reply, "mstt", 200);      /* 12 */

  CHECK_ERR(L_DACP, evbuffer_add_buffer(reply, proplist));

  evbuffer_free(proplist);

  httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, 0);

  return;

 out_free_proplist:
  evbuffer_free(proplist);

 out_free_propstr:
  free(propstr);
}

static void
dacp_reply_setproperty(struct evbuffer *reply, struct dacp_request *dreq)
{
  const struct dacp_prop_map *dpm;
  struct evkeyval *param;
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  /* Known properties:
   * dacp.shufflestate 0/1
   * dacp.repeatstate  0/1/2
   * dacp.playingtime  seek to time in ms
   * dmcp.volume       0-100, float
   */

  /* /ctrl-int/1/setproperty?dacp.shufflestate=1&session-id=100 */

  TAILQ_FOREACH(param, dreq->query, next)
    {
      dpm = dacp_find_prop(param->key, strlen(param->key));

      if (!dpm)
	{
	  DPRINTF(E_SPAM, L_DACP, "Unknown DACP property %s\n", param->key);
	  continue;
	}

      if (dpm->propset)
	dpm->propset(param->value, dreq->query);
      else
	DPRINTF(E_WARN, L_DACP, "No setter method for DACP property %s\n", dpm->desc);
    }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static void
dacp_reply_getspeakers(struct evbuffer *reply, struct dacp_request *dreq)
{
  struct evbuffer *spklist;
  size_t len;
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  CHECK_NULL(L_DACP, spklist = evbuffer_new());

  player_speaker_enumerate(speaker_enum_cb, spklist);

  len = evbuffer_get_length(spklist);
  dmap_add_container(reply, "casp", 12 + len);
  dmap_add_int(reply, "mstt", 200); /* 12 */

  evbuffer_add_buffer(reply, spklist);

  evbuffer_free(spklist);

  httpd_send_reply(dreq->req, HTTP_OK, "OK", reply, 0);
}

static void
dacp_reply_setspeakers(struct evbuffer *reply, struct dacp_request *dreq)
{
  const char *param;
  const char *ptr;
  uint64_t *ids;
  int nspk;
  int i;
  int ret;

  ret = dacp_request_authorize(dreq);
  if (ret < 0)
    return;

  param = evhttp_find_header(dreq->query, "speaker-id");
  if (!param)
    {
      DPRINTF(E_LOG, L_DACP, "Missing speaker-id parameter in DACP setspeakers request\n");

      httpd_send_error(dreq->req, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  if (strlen(param) == 0)
    {
      ids = NULL;
      goto fastpath;
    }

  nspk = 1;
  ptr = param;
  while ((ptr = strchr(ptr + 1, ',')))
    nspk++;

  ids = calloc((nspk + 1), sizeof(uint64_t));
  if (!ids)
    {
      DPRINTF(E_LOG, L_DACP, "Out of memory for speaker ids\n");

      httpd_send_error(dreq->req, HTTP_SERVUNAVAIL, "Internal Server Error");
      return;
    }

  param--;
  i = 1;
  do
    {
      param++;

      // Some like Remote will give us hex, others will give us decimal (e.g. Hyperfine)
      if (strncmp(param, "0x", 2) == 0)
	ret = safe_hextou64(param, &ids[i]);
      else
	ret = safe_atou64(param, &ids[i]);

      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DACP, "Invalid speaker id in request: %s\n", param);

	  nspk--;
	  continue;
	}
      else
	{
	  DPRINTF(E_DBG, L_DACP, "Speaker id converted with ret %d, param %s, dec val %" PRIu64 ".\n", ret, param, ids[i]);
	}
      i++;
    }
  while ((param = strchr(param + 1, ',')));

  ids[0] = nspk;

 fastpath:
  ret = player_speaker_set(ids);

  if (ids)
    free(ids);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Speakers de/activation failed!\n");

      /* Password problem */
      if (ret == -2)
	httpd_send_error(dreq->req, 902, "");
      else
	httpd_send_error(dreq->req, 500, "Internal Server Error");

      return;
    }

  /* 204 No Content is the canonical reply */
  httpd_send_reply(dreq->req, HTTP_NOCONTENT, "No Content", reply, HTTPD_SEND_NO_GZIP);
}

static struct uri_map dacp_handlers[] =
  {
    {
      .regexp = "^/ctrl-int$",
      .handler = dacp_reply_ctrlint
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/cue$",
      .handler = dacp_reply_cue
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/playspec$",
      .handler = dacp_reply_playspec
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/pause$",
      .handler = dacp_reply_pause
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/playpause$",
      .handler = dacp_reply_playpause
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/nextitem$",
      .handler = dacp_reply_nextitem
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/previtem$",
      .handler = dacp_reply_previtem
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/beginff$",
      .handler = dacp_reply_beginff
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/beginrew$",
      .handler = dacp_reply_beginrew
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/playresume$",
      .handler = dacp_reply_playresume
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/playstatusupdate$",
      .handler = dacp_reply_playstatusupdate
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/playqueue-contents$",
      .handler = dacp_reply_playqueuecontents
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/playqueue-edit$",
      .handler = dacp_reply_playqueueedit
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/nowplayingartwork$",
      .handler = dacp_reply_nowplayingartwork
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/getproperty$",
      .handler = dacp_reply_getproperty
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/setproperty$",
      .handler = dacp_reply_setproperty
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/getspeakers$",
      .handler = dacp_reply_getspeakers
    },
    {
      .regexp = "^/ctrl-int/[[:digit:]]+/setspeakers$",
      .handler = dacp_reply_setspeakers
    },
    {
      .regexp = NULL,
      .handler = NULL
    }
  };


/* ------------------------------- DACP API --------------------------------- */

void
dacp_request(struct evhttp_request *req, struct httpd_uri_parsed *uri_parsed)
{
  struct dacp_request *dreq;
  struct evbuffer *reply;
  struct evkeyvalq *headers;

  DPRINTF(E_DBG, L_DACP, "DACP request: '%s'\n", uri_parsed->uri);

  dreq = dacp_request_parse(req, uri_parsed);
  if (!dreq)
    {
      httpd_send_error(req, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  headers = evhttp_request_get_output_headers(req);
  evhttp_add_header(headers, "DAAP-Server", "forked-daapd/" VERSION);
  /* Content-Type for all DACP replies; can be overriden as needed */
  evhttp_add_header(headers, "Content-Type", "application/x-dmap-tagged");

  CHECK_NULL(L_DACP, reply = evbuffer_new());

  dreq->handler(reply, dreq);

  evbuffer_free(reply);
  free(dreq);
}

int
dacp_is_request(const char *path)
{
  if (strncmp(path, "/ctrl-int/", strlen("/ctrl-int/")) == 0)
    return 1;
  if (strcmp(path, "/ctrl-int") == 0)
    return 1;

  return 0;
}

int
dacp_init(void)
{
  char buf[64];
  int i;
  int ret;

  current_rev = 2;
  update_requests = NULL;

#ifdef HAVE_EVENTFD
  update_efd = eventfd(0, EFD_CLOEXEC);
  if (update_efd < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not create update eventfd: %s\n", strerror(errno));

      return -1;
    }
#else
# ifdef HAVE_PIPE2
  ret = pipe2(update_pipe, O_CLOEXEC);
# else
  ret = pipe(update_pipe);
# endif
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DACP, "Could not create update pipe: %s\n", strerror(errno));

      return -1;
    }
#endif /* HAVE_EVENTFD */

  for (i = 0; dacp_handlers[i].handler; i++)
    {
      ret = regcomp(&dacp_handlers[i].preg, dacp_handlers[i].regexp, REG_EXTENDED | REG_NOSUB);
      if (ret != 0)
        {
          regerror(ret, &dacp_handlers[i].preg, buf, sizeof(buf));

          DPRINTF(E_FATAL, L_DACP, "DACP init failed; regexp error: %s\n", buf);
	  goto regexp_fail;
        }
    }

#ifdef HAVE_EVENTFD
  updateev = event_new(evbase_httpd, update_efd, EV_READ, playstatusupdate_cb, NULL);
#else
  updateev = event_new(evbase_httpd, update_pipe[0], EV_READ, playstatusupdate_cb, NULL);
#endif
  if (!updateev)
    {
      DPRINTF(E_LOG, L_DACP, "Could not create update event\n");

      return -1;
    }
  event_add(updateev, NULL);

  seek_timer = evtimer_new(evbase_httpd, seek_timer_cb, NULL);
  if (!seek_timer)
    {
      DPRINTF(E_LOG, L_DACP, "Could not create seek_timer event\n");

      return -1;
    }

  listener_add(dacp_playstatus_update_handler, LISTENER_PLAYER);

  return 0;

 regexp_fail:
#ifdef HAVE_EVENTFD
  close(update_efd);
#else
  close(update_pipe[0]);
  close(update_pipe[1]);
#endif
  return -1;
}

void
dacp_deinit(void)
{
  struct dacp_update_request *ur;
  struct evhttp_connection *evcon;
  int i;

  listener_remove(dacp_playstatus_update_handler);

  event_free(seek_timer);

  for (i = 0; dacp_handlers[i].handler; i++)
    regfree(&dacp_handlers[i].preg);

  for (ur = update_requests; update_requests; ur = update_requests)
    {
      update_requests = ur->next;

      evcon = evhttp_request_get_connection(ur->req);
      if (evcon)
	{
	  evhttp_connection_set_closecb(evcon, NULL, NULL);
	  evhttp_connection_free(evcon);
	}

      free(ur);
    }

  event_free(updateev);

#ifdef HAVE_EVENTFD
  close(update_efd);
#else
  close(update_pipe[0]);
  close(update_pipe[1]);
#endif
}
