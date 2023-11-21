/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
 *  revised:  11/19/23 brent@mbari.org
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <ctype.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#define DEFAULT_RTSP_PORT "8554"
#define DEFAULT_ENDPOINT "video"

static char *port = (char *) DEFAULT_RTSP_PORT;
static char *mount = (char *) "/" DEFAULT_ENDPOINT;
static char *retransmitTime = NULL; //do-retransmission and set time (ms)
static char *profiles = NULL;       //default: 'AVP'

#if GST_VERSION_MINOR >= 20
static gboolean disable_rtcp = FALSE;
#endif

static GOptionEntry entries[] = {
  {"port", 'p', 0, G_OPTION_ARG_STRING, &port,
      "Port to listen on (default: " DEFAULT_RTSP_PORT ")", "PORT"},
  {"endpoint", 'e', 0, G_OPTION_ARG_STRING, &mount+1,
      "URI end point (default: " DEFAULT_ENDPOINT ")", "Sevice Name"},
  {"rtsp-profiles", 'r', 0, G_OPTION_ARG_STRING, &profiles,
      "Allowed transfer profiles (default: AVP)", "AVP+AVPF+SAVP+SAVPF"},
  {"retransmission-time", 't', 0, G_OPTION_ARG_STRING, &retransmitTime,
      "Milliseconds to retain packets for retransmission\n"
      "      <also sets do-retransmission flag>", "ms"},
#if GST_VERSION_MINOR >= 20
  {"disable-rtcp", '\0', 0, G_OPTION_ARG_NONE, &disable_rtcp,
      "Disable RTCP", NULL},
#endif
  {NULL}
};

static char *parseProfile (char *base, GstRTSPProfile *result)
/*
 * Parse single profile pointed to by next
 * returns pointer to first char not part of profile
 * (non-zero) profile mask or'ed into *result if profile recognized
 */
{
  char *cursor = base;
  GstRTSPProfile profile;
  if (toupper(*base) == 'S')
    cursor++;
  if (strncasecmp(cursor, "AVP", 3))
    return cursor;
  if (toupper(cursor[3]) == 'F')
    profile = cursor++==base ? GST_RTSP_PROFILE_AVPF : GST_RTSP_PROFILE_SAVPF;
  else
    profile = cursor==base ? GST_RTSP_PROFILE_AVP : GST_RTSP_PROFILE_SAVP;
  *result |= profile;
  return cursor+3;
}

static void
media_constructed (GstRTSPMediaFactory * factory, GstRTSPMedia * media)
{
  guint i, n_streams = gst_rtsp_media_n_streams (media);

  for (i = 0; i < n_streams; i++) {
    GstRTSPStream *stream = gst_rtsp_media_get_stream (media, i);
    g_printerr("%d:%s retransmission_time = %ld\n", i,
       gst_rtsp_stream_is_sender(stream) ? "Sender":"Receiver",
       gst_rtsp_stream_get_retransmission_time(stream));
  }
  gst_rtsp_media_set_do_retransmission(media, TRUE);
}


/* this timeout is periodically run to clean up the expired sessions from the
 * pool. This needs to be run explicitly currently but might be done
 * automatically as part of the mainloop. */
static gboolean
timeout (GstRTSPServer * server)
{
  GstRTSPSessionPool *pool;

  pool = gst_rtsp_server_get_session_pool (server);
  gst_rtsp_session_pool_cleanup (pool);
  g_object_unref (pool);

  return TRUE;
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GstRTSPServer *server;
  GstRTSPMountPoints *mounts;
  GstRTSPMediaFactory *factory;
  GOptionContext *optctx;
  GError *error = NULL;
  char *end;

  g_print("Launch RTSP Server -- 11/21/23 brent@mbari.org\n");
  optctx = g_option_context_new (
    "\"Launch Line\"\n"
    "Example Launch Line:\
  \"( videotestsrc ! x264enc ! rtph264pay name=pay0 pt=96 )\"");
  g_option_context_add_main_entries (optctx, entries, NULL);
  g_option_context_add_group (optctx, gst_init_get_option_group ());
  if (!g_option_context_parse (optctx, &argc, &argv, &error)) {
    g_printerr ("Error parsing options: %s\n", error->message);
    g_option_context_free (optctx);
    g_clear_error (&error);
    return -1;
  }
  g_option_context_free (optctx);

  if (!argv[1]) {
      g_printerr("Error: empty pipeline\n");
      return -1;
  }

  loop = g_main_loop_new (NULL, FALSE);

  /* create a server instance */
  server = gst_rtsp_server_new ();
  g_object_set (server, "service", port, NULL);

  /* get the mount points for this server, every server has a default object
   * that be used to map uri mount points to media factories */
  mounts = gst_rtsp_server_get_mount_points (server);

  /* make a media factory for a test stream. The default media factory can use
   * gst-launch syntax to create pipelines.
   * any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */
  factory = gst_rtsp_media_factory_new ();

  if (profiles) {
    GstRTSPProfile mask = 0;
    char *cursor = profiles;
    while (*cursor) {
      GstRTSPProfile profile = 0;
      cursor=parseProfile(cursor, &profile);
      if (!profile) {
badProfiles:
        g_printerr("Unknown RTSP profiles (\"%s\") specified\n", profiles);
        return 3;
      }
      mask |= profile;
      if (!*cursor)
        break;
      if (isalnum(*cursor))
        goto badProfiles;
      cursor++;
    }
    gst_rtsp_media_factory_set_profiles(factory, mask);
  }

  if (retransmitTime) {
    guint64 retransMs = strtoull(retransmitTime, &end, 0);
    if (*end || end == retransmitTime) {
        g_printerr("Invalid retransmission time (\"%s\") specified\n",
                    retransmitTime);
        return 4;
    }
    gst_rtsp_media_factory_set_retransmission_time(
        factory, retransMs * GST_MSECOND);
  }

#if GST_VERSION_MINOR >= 20
  gst_rtsp_media_factory_set_enable_rtcp (factory, !disable_rtcp);
#endif

  gst_rtsp_media_factory_set_launch (factory, argv[1]);
  gst_rtsp_media_factory_set_shared (factory, TRUE);
#if 0
  g_signal_connect (factory, "media-constructed", (GCallback)
      media_constructed, NULL);
  g_signal_connect (factory, "media-configure", (GCallback)
      media_constructed, NULL);
#endif
  g_print("Pipeline: %s\n", gst_rtsp_media_factory_get_launch(factory));

  /* attach the mount url */
  gst_rtsp_mount_points_add_factory (mounts, mount, factory);

  /* don't need the ref to the mapper anymore */
  g_object_unref (mounts);

  /* attach the server to the default maincontext */
  if (!gst_rtsp_server_attach (server, NULL)) {
    g_print ("failed to attach the server\n");
    return 6;
  }

  /* add a timeout for the session cleanup */
  g_timeout_add_seconds (5, (GSourceFunc) timeout, server);

  /* start serving */
  g_print ("Stream ready at rtsp://127.0.0.1:%s%s\n", port, mount);
  g_main_loop_run (loop);

  return 0;
}
