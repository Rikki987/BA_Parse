#include <string.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <pthread.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <gdk/gdk.h>
#if defined (GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#elif defined (GDK_WINDOWING_WIN32)
#include <gdk/gdkwin32.h>
#elif defined (GDK_WINDOWING_QUARTZ)
#include <gdk/gdkquartz.h>
#endif


#define DEFAULT_RTSP_PORT "8554"
static char *port= (char *) DEFAULT_RTSP_PORT;

static GstVideoOverlay *overlay;
//static GstVideoOverlay *overlay2;

/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData {
  GstElement *playbin;           /* Our one and only pipeline */
  GstElement *playbin2;           /* Our one and only pipeline */

  GstElement *queue; 		 /* Queue element to monitor if pipeline is alive */

  GtkWidget *slider;              /* Slider widget to keep track of current position */
  GtkWidget *streams_list;        /* Text widget to display info about the streams */
  gulong slider_update_signal_id; /* Signal ID for the slider update signal */

  GstState state;                 /* Current state of the pipeline */
  gint64 duration;                /* Duration of the clip, in nanoseconds */

  gboolean terminate;             /* Should we terminate execution? */
  gboolean playing; 		  /* */

  GstBus *bus2;
  GstBus *bus;
} CustomData;

/* global window_handle pointer (needed for linking our glimagsink to the gui window */
static guintptr window_handle = 0;

static gboolean rtsp_client (CustomData *data);

static void underrun_handler(GstBus* bus, GstMessage *msg, CustomData *data) {
  g_print ("Underrun message occured!");
//  gst_element_set_state (data->playbin, GST_STATE_PAUSED);
}

static void running_handler(GstBus* bus, GstMessage *msg, CustomData *data) {
  g_print ("Running message occured!");
  //gst_element_set_state (data->playbin, GST_STATE_PLAYING);
}

/* This function is called when the glimagesink element posts a prepare-window-handle message
 * -> bus sync handler will be called from the streaming thread directly
 * in this function we tell glimagesink to render on existing application window (window_handle)
 * needed because glimagesink element itself is created asynchronously from a GStreamer streaming thread some time after the pipeline has been started up */
static GstBusSyncReply bus_sync_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
 // ignore anything but 'prepare-window-handle' element messages
 if (!gst_is_video_overlay_prepare_window_handle_message (message))
   return GST_BUS_PASS;

 if (window_handle != 0) {
  // GstVideoOverlay *overlay;

   // GST_MESSAGE_SRC (message) will be the video sink element
   overlay = GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message));
   gst_video_overlay_set_window_handle (overlay, window_handle);
 } else {
   g_warning ("Should have obtained window_handle by now!");
 }

 gst_message_unref (message);
 return GST_BUS_DROP;
}

/* This function is called when the GUI toolkit creates the physical window that will hold the video.
 * At this point we can retrieve its handler (which has a different meaning depending on the windowing system)
 * and pass it to GStreamer through the VideoOverlay interface. */
static void realize_cb (GtkWidget *widget, CustomData *data) {
 // g_object_get (data->playbin->sink, "widget", &widget, NULL);
  GdkWindow *window = gtk_widget_get_window (widget);

  if (!gdk_window_ensure_native (window))
    g_error ("Couldn't create native window needed for GstVideoOverlay!");

  /* Retrieve window handler from GDK */
#if defined (GDK_WINDOWING_WIN32)
  window_handle = (guintptr)GDK_WINDOW_HWND (window);
//  window_handle2 = (guintptr)GDK_WINDOW_HWND (window);
#elif defined (GDK_WINDOWING_QUARTZ)
  window_handle = gdk_quartz_window_get_nsview (window);
 // window_handle2 = gdk_quartz_window_get_nsview (window);
#elif defined (GDK_WINDOWING_X11)
  window_handle = GDK_WINDOW_XID (window);
//  window_handle2 = GDK_WINDOW_XID (window);
#endif

 // g_object_get (data->playbin->sink, "widget", &widget, NULL);

  /* Pass it to playbin, which implements VideoOverlay and will forward it to the video sink */
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->playbin2), window_handle);
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->playbin), window_handle);
}

/* This function is called when the PLAY button is clicked */
static void play_cb (GtkButton *button, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_PLAYING);
}


/* This function is called when the PAUSE button is clicked */
static void pause_cb (GtkButton *button, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_PAUSED);
}

/* This function is called when the STOP button is clicked */
static void stop_cb (GtkButton *button, CustomData *data) {
  //gst_element_set_state (data->playbin, GST_STATE_NULL);
  gst_element_set_state (data->playbin2, GST_STATE_READY);
  gst_element_set_state (data->playbin, GST_STATE_READY);

}

/* This function is called when the main window is closed */
static void delete_event_cb (GtkWidget *widget, GdkEvent *event, CustomData *data) {
  stop_cb (NULL, data);
  overlay=NULL;
  gtk_main_quit ();
}

/* This function is called everytime the video window needs to be redrawn (due to damage/exposure,
 * rescaling, etc). GStreamer takes care of this in the PAUSED and PLAYING states, otherwise,
 * we simply draw a black rectangle to avoid garbage showing up. */
static gboolean draw_cb (GtkWidget *widget, cairo_t *cr, CustomData *data) {
  GtkAllocation allocation;
  if (data->state < GST_STATE_PAUSED) {
  gtk_widget_get_allocation(widget, &allocation);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_rectangle (cr, 0, 0, 1900, 1040);
    cairo_fill (cr);
  }

 return FALSE;
}

/* This function is called when the slider changes its position. We perform a seek to the
 * new position here. */
static void slider_cb (GtkRange *range, CustomData *data) {
  gdouble value = gtk_range_get_value (GTK_RANGE (data->slider));
  gst_element_seek_simple (data->playbin, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
      (gint64)(value * GST_SECOND));
}

/* This creates all the GTK+ widgets that compose our application, and registers the callbacks */
static void create_ui (CustomData *data) {
  GtkWidget *main_window;  /* The uppermost window, containing all other windows */
  GtkWidget *video_window; /* The drawing area where the video will be shown */
  GtkWidget *main_box;     /* VBox to hold main_hbox and the controls */
  GtkWidget *main_hbox;    /* HBox to hold the video_window and the stream info text widget */
  GtkWidget *controls;     /* HBox to hold the buttons and the slider */
  GtkWidget *play_button, *pause_button, *stop_button; /* Buttons */

  main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), data);

  video_window = gtk_drawing_area_new();
  gtk_widget_set_double_buffered (video_window, FALSE);
  g_signal_connect (video_window, "realize", G_CALLBACK (realize_cb), data);
  g_signal_connect (video_window, "draw", G_CALLBACK (draw_cb), data);

  play_button = gtk_button_new_from_icon_name ("media-playback-start", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (play_button), "clicked", G_CALLBACK (play_cb), data);

  pause_button = gtk_button_new_from_icon_name ("media-playback-pause", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (pause_button), "clicked", G_CALLBACK (pause_cb), data);

  stop_button = gtk_button_new_from_icon_name ("media-playback-stop", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (stop_button), "clicked", G_CALLBACK (stop_cb), data);

  data->slider = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_scale_set_draw_value (GTK_SCALE (data->slider), 0);
  data->slider_update_signal_id = g_signal_connect (G_OBJECT (data->slider), "value-changed", G_CALLBACK (slider_cb), data);

  data->streams_list = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (data->streams_list), FALSE);

  controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (controls), play_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), pause_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), stop_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), data->slider, TRUE, TRUE, 2);

  main_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), video_window, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), data->streams_list, FALSE, FALSE, 2);

  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start (GTK_BOX (main_box), main_hbox, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (main_box), controls, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (main_window), main_box);
  gtk_window_set_default_size (GTK_WINDOW (main_window), 1920, 1080);

  gtk_widget_show_all (main_window);
}

/* This function is called periodically to refresh the GUI */
static gboolean refresh_ui (CustomData *data) {
  gint64 current = -1;

  /* We do not want to update anything unless we are in the PAUSED or PLAYING states */
  if (data->state < GST_STATE_PAUSED)
    return TRUE;

  /* If we didn't know it yet, query the stream duration */
  if (!GST_CLOCK_TIME_IS_VALID (data->duration)) {
    if (!gst_element_query_duration (data->playbin, GST_FORMAT_TIME, &data->duration)) {
      g_printerr ("Could not query current duration.\n");
    } else {
      /* Set the range of the slider to the clip duration, in SECONDS */
      gtk_range_set_range (GTK_RANGE (data->slider), 0, (gdouble)data->duration / GST_SECOND);
    }
  }

  if (gst_element_query_position (data->playbin, GST_FORMAT_TIME, &current)) {
    /* Block the "value-changed" signal, so the slider_cb function is not called
     * (which would trigger a seek the user has not requested) */
    g_signal_handler_block (data->slider, data->slider_update_signal_id);
    /* Set the position of the slider to the current pipeline positoin, in SECONDS */
    gtk_range_set_value (GTK_RANGE (data->slider), (gdouble)current / GST_SECOND);
    /* Re-enable the signal */
    g_signal_handler_unblock (data->slider, data->slider_update_signal_id);
  }
  return TRUE;
}

/* This function is called when new metadata is discovered in the stream */
static void tags_cb (GstElement *playbin, gint stream, CustomData *data) {
  /* We are possibly in a GStreamer working thread, so we notify the main
   * thread of this event through a message in the bus */
  gst_element_post_message (playbin,
    gst_message_new_application (GST_OBJECT (playbin),
      gst_structure_new_empty ("tags-changed")));
}


/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) */
static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  g_print ("End-Of-Stream reached.\n");
  gst_element_set_state (data->playbin, GST_STATE_READY);
  gst_element_set_state (data->playbin, GST_STATE_NULL);
  g_timeout_add_seconds (10, (GSourceFunc) rtsp_client, data);

}
/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) */
static void eos_cb2 (GstBus *bus, GstMessage *msg, CustomData *data) {
  g_print ("\n\n End-Of-Stream waitung video reached.\n");
  gst_element_set_state (data->playbin2, GST_STATE_READY);
  sleep(5);
  g_print ("Trying to restart video");
  gst_element_set_state (data->playbin2, GST_STATE_PAUSED);
  gst_element_set_state (data->playbin2, GST_STATE_PLAYING);
}

/* This function is called when the pipeline changes states. We use it to
 * keep track of the current state. */
static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstState old_state, new_state, pending_state;
  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->playbin)) {
    data->state = new_state;
    g_print ("State set to %s\n", gst_element_state_get_name (new_state));
    if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) {
      /* For extra responsiveness, we refresh the GUI as soon as we reach the PAUSED state */
      refresh_ui (data);
    }
  }
  if (new_state == GST_STATE_PLAYING)
  {
   gst_element_set_state (data->playbin2, GST_STATE_PAUSED);
  }
  if (new_state == GST_STATE_PAUSED || new_state == GST_STATE_READY )
   {
   gst_element_set_state (data->playbin2, GST_STATE_PLAYING);
  }
}

/* This function is called when an error message is posted on the bus */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;
  GstStateChangeReturn ret;
  char *err_elem;
  char *err_msg;
  GError *error=NULL;


  /* Set the pipeline to READY (which stops playback) */
//  gst_element_set_state (data->playbin2, GST_STATE_READY);
//  gst_element_set_state (data->playbin2, GST_STATE_NULL);
  gst_element_set_state (data->playbin, GST_STATE_READY);
//  gst_element_set_state (data->playbin, GST_STATE_NULL);


  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  err_elem=GST_OBJECT_NAME(msg->src);
  err_msg=err->message;

  if ((strncmp (err_elem, "rtspsrc",7) ==0)) 
  {
    g_printerr ("Rtspsrc detected");
    /* Stop program */
    //gtk_main_quit();
    gst_element_set_state (data->playbin, GST_STATE_NULL);

    /* Set pipeline 2 to playing state */
    gst_element_set_state (data->playbin2, GST_STATE_PLAYING);

    g_timeout_add_seconds (10, (GSourceFunc) rtsp_client, data);
    if (strncmp (debug_info, "gstrtspsrc.c(4825)",18)==0)
    {
     //gst_element_set_state (data->playbin, GST_STATE_NULL);
    }
  }

  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);
}


/* This thread handles the startup and running of the rtsp server side */
void rtsp_server(void)
{
  GMainLoop *loop;
  GstRTSPServer *server;
  GstRTSPMountPoints *mounts;
  GstRTSPMediaFactory *factory;

  /* create main loop */
  loop = g_main_loop_new (NULL, FALSE);

  /* create a server instance */
  server = gst_rtsp_server_new ();
  gst_rtsp_server_set_address(server, "10.252.61.91");
  g_object_set (server, "service", port, NULL);

  /* get the mount points for this server, every server has a default object
   * that be used to map uri mount points to media factories */
  mounts = gst_rtsp_server_get_mount_points (server);

  /* make a media factory for a test stream. The default media factory can use
   * gst-launch syntax to create pipelines.
   * any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */
  factory = gst_rtsp_media_factory_new ();
  gst_rtsp_media_factory_set_launch (factory, "v4l2src ! video/x-raw, width=1920, height=1080, framerate=30/1 ! queue max-size-buffers=1 leaky=downstream ! omxh264enc ! video/x-h264, stream-format=byte-stream, alignment=au, profile=high ! h264parse ! rtph264pay name=pay0 pt=96 ");

  /* make rtsp-server available for multiple clients */
  gst_rtsp_media_factory_set_shared(factory, TRUE);

  /* attach the test factory to the /test url */
  gst_rtsp_mount_points_add_factory (mounts, "/test", factory);

  /* don't need the ref to the mapper anymore */
  g_object_unref (mounts);

  /* attach the server to the default maincontext */
  gst_rtsp_server_attach (server, NULL);

  /* start serving */
  g_print ("stream ready at rtsp://10.252.61.91:%s/test\n", port);
}

static gboolean start_streaming (CustomData *data)
{
  GstStateChangeReturn ret;

  /* Start playing */
  ret=gst_element_set_state (data->playbin, GST_STATE_PLAYING);
  if (ret==GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (data->playbin);
    return;
  }
 //window_handle2=NULL;
// gst_element_set_state (data->playbin2, GST_STATE_READY);
// data->playbin2=NULL;
// data->bus2=NULL;

/* return FALSE to make sure function is only called once */
 return FALSE;
}

static gboolean rtsp_client (CustomData *data)
{
  GError *error=NULL;
  GstStateChangeReturn ret;
  gchar *pipe_desc;

//  gst_object_unref(data->playbin);
  //gst_object_unref(data->bus);
  data->playbin=NULL;
//  data->bus=NULL;


  g_print ("Start client stream ! ");

  /* Create the elements */
  pipe_desc= g_strdup_printf ("rtspsrc location=rtsp://10.252.61.135:8554/test latency=0 do-retransmission=false user-id=user user-pw=password ! queue name=q ! rtpjitterbuffer latency=10 drop-on-latency=true mode=2 ! application/x-rtp, encoding-name=H264 ! rtph264depay ! h264parse ! capsfilter caps='video/x-h264, stream-format=byte-stream, frame-rate=30/1' ! omxh264dec ! glimagesink sync=false async=false"); 
 // pipe_desc= g_strdup_printf ("rtspsrc location=rtsp://169.254.197.5:8554/test latency=0 do-retransmission=false user-id=user user-pw=password ! queue name=q ! rtpjitterbuffer latency=10 drop-on-latency=true mode=2 ! application/x-rtp, encoding-name=H264 ! rtph264depay ! h264parse ! capsfilter caps='video/x-h264, stream-format=byte-stream, frame-rate=30/1' ! omxh264dec ! glimagesink sync=false async=false"); 
  data->playbin=gst_parse_launch(pipe_desc, &error);
  g_free(pipe_desc);
  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  data->bus = gst_element_get_bus (data->playbin);
  gst_bus_set_sync_handler (data->bus, (GstBusSyncHandler) bus_sync_handler, NULL, NULL);

  gst_bus_add_signal_watch (data->bus);
  g_signal_connect (G_OBJECT (data->bus), "message::error", (GCallback)error_cb, data);
  g_signal_connect (G_OBJECT (data->bus), "message::eos", (GCallback)eos_cb, data);
  g_signal_connect (G_OBJECT (data->bus), "message::state-changed", (GCallback)state_changed_cb, data);

  gst_object_unref (data->bus);
  /* Start playing */
  ret=gst_element_set_state (data->playbin, GST_STATE_PLAYING);
  if (ret==GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (data->playbin);
    return;
  }
  /* return FALSE to make sure function is only called once */
  return FALSE;
}

int main(int argc, char *argv[]) {
  CustomData data;
  GstStateChangeReturn ret;
  //GstBus *bus;
  gchar *pipe_desc;
  gchar *pipe_desc2;
  GError *error=NULL;
  GstMessage *msg;
  pthread_t rtsp_server_thread, rtsp_client_thread;

  /* Initialize GTK */
  gtk_init (&argc, &argv);

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  /* Start rtsp-server-thread */
  //  pthread_create ( &rtsp_server_thread, NULL, (void* )rtsp_server, NULL );

  /* Initialize our data structure */
  memset (&data, 0, sizeof (data));
  data.duration = GST_CLOCK_TIME_NONE;

  /* Create the elements */
  pipe_desc= g_strdup_printf ("rtspsrc location=rtsp://10.252.61.135:8554/test latency=0 do-retransmission=false user-id=user user-pw=password ! queue name=q ! rtpjitterbuffer latency=10 drop-on-latency=true mode=2 ! application/x-rtp, encoding-name=H264 ! rtph264depay ! h264parse ! capsfilter caps='video/x-h264, stream-format=byte-stream, frame-rate=30/1' ! omxh264dec ! glimagesink sync=false async=false"); 
 // pipe_desc= g_strdup_printf ("rtspsrc location=rtsp://169.254.197.5:8554/test latency=0 do-retransmission=false user-id=user user-pw=password ! queue name=q ! rtpjitterbuffer latency=10 drop-on-latency=true mode=2 ! application/x-rtp, encoding-name=H264 ! rtph264depay ! h264parse ! capsfilter caps='video/x-h264, stream-format=byte-stream, frame-rate=30/1' ! omxh264dec ! glimagesink sync=false async=false"); 
  data.playbin=gst_parse_launch(pipe_desc, &error);
  g_free(pipe_desc);

  error=NULL;
  //pipe_desc2= g_strdup_printf ("videotestsrc pattern=1 ! glimagesink ");
  pipe_desc2= g_strdup_printf ("multifilesrc location=/home/pi/Videos/test.h264 loop=true ! decodebin ! glimagesink ");
  data.playbin2=gst_parse_launch(pipe_desc2, &error);
  g_free(pipe_desc2);

  /* Create the GUI */
  create_ui (&data);

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  data.bus = gst_element_get_bus (data.playbin);
  gst_bus_set_sync_handler (data.bus, (GstBusSyncHandler) bus_sync_handler, NULL, NULL);

  gst_bus_add_signal_watch (data.bus);
  g_signal_connect (G_OBJECT (data.bus), "message::error", (GCallback)error_cb, &data);
  g_signal_connect (G_OBJECT (data.bus), "message::eos", (GCallback)eos_cb, &data);
  g_signal_connect (G_OBJECT (data.bus), "message::state-changed", (GCallback)state_changed_cb, &data);

  gst_object_unref (data.bus);

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  data.bus2 = gst_element_get_bus (data.playbin2);
  gst_bus_set_sync_handler (data.bus2, (GstBusSyncHandler) bus_sync_handler, NULL, NULL);

  gst_bus_add_signal_watch (data.bus2);
  g_signal_connect (G_OBJECT (data.bus2), "message::eos", (GCallback)eos_cb2, &data);

  gst_object_unref (data.bus2);

  /* Monitor if pipeline is alive -> monitor "underrun" signal from queue element*/
  //  data.queue= gst_bin_get_by_name(data.playbin, "q");
  //  g_signal_connect (data.queue, "underrun", G_CALLBACK(underrun_handler), &data);
  //  g_signal_connect (data.queue, "running", G_CALLBACK(running_handler), &data);


  /* Start playing */
  ret=gst_element_set_state (data.playbin2, GST_STATE_PLAYING);
  if (ret==GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (data.playbin2);
    return;
  }

  /* Register a function that GLib will call every second */
  g_timeout_add_seconds (1, (GSourceFunc)refresh_ui, &data);

  /* Register a function that GLib will call once after 10 seconds */
  g_timeout_add_seconds (10, (GSourceFunc) start_streaming, &data);

  /* Start the GTK main loop. We will not regain control until gtk_main_quit is called. */
  gtk_main ();

  /* Free resources */
  gst_element_set_state (data.playbin, GST_STATE_NULL);
  gst_object_unref (data.playbin);
  gst_element_set_state (data.playbin2, GST_STATE_NULL);
  gst_object_unref (data.playbin2);
  return 0;
}
