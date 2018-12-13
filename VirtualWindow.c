#include <string.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

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

/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData {
  GstElement *videoStreamUschl;           /* Pipeline for the live stream from Unterschleißheim */
  GstElement *waitingVideoUschl;          /* Pipeline for waiting video for Unterschleißheim video window */

  GstElement *videoStreamUlm;			 /* Pipeline for the live stream from Ulm */
  GstElement *waitingVideoUlm;			 /* Pipeline for waiting video for Ulm video window */

  GstState stateStreamUschl;                 /* Current state of the pipeline */
  GstState stateStreamUlm;                 /* Current state of the pipeline */
  gint64 duration;                /* Duration of the clip, in nanoseconds */

  GstBus *busWaitingUschl;				/* pipeline bus monitoring Unterschleißheim waiting stream */
  GstBus *busStreamUschl;				/* pipeline bus monitoring Unterschleißheim stream */
  GstBus *busStreamUlm;		/* pipeline bus monitoring Ulm stream */
  GstBus *busWaitingUlm;		/* pipeline bus monitoring Ulm waiting stream */
} CustomData;

/* global window_handle pointer (needed for linking our glimagsink to the gui window */
static guintptr window_handle_Uschl = 0;
static guintptr window_handle_Ulm = 0;

/* Definition of functions to start video streams */
static gboolean rtsp_client_Uschl (CustomData *data);
static gboolean rtsp_client_Ulm (CustomData *data);

/* This function is called when the glimagesink element posts a prepare-window-handle message
 * -> bus sync handler will be called from the streaming thread directly
 * in this function we tell glimagesink to render on existing application window (window_handle)
 * needed because glimagesink element itself is created asynchronously from a GStreamer streaming thread some time after the pipeline has been started up */
static GstBusSyncReply bus_sync_handler_Uschl (GstBus * bus, GstMessage * message, gpointer user_data){
   GstVideoOverlay *overlay;
 /* ignore anything but 'prepare-window-handle' element messages */
 if (!gst_is_video_overlay_prepare_window_handle_message (message)){
   return GST_BUS_PASS;
 }
 if (window_handle_Uschl != 0) {
   /* GST_MESSAGE_SRC (message) will be the video sink element */
   overlay = GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message));
   gst_video_overlay_set_window_handle (overlay, window_handle_Uschl);
 } else {
   g_warning ("Should have obtained window_handle by now!");
 }
 gst_message_unref (message);
 return GST_BUS_DROP;
}

/* Same function for second video window */
static GstBusSyncReply bus_sync_handler_Ulm (GstBus * bus, GstMessage * message, gpointer user_data){
	GstVideoOverlay *overlay;
 /* ignore anything but 'prepare-window-handle' element messages */
 if (!gst_is_video_overlay_prepare_window_handle_message (message)){
   return GST_BUS_PASS;
 }
 if (window_handle_Ulm != 0) {
   /* GST_MESSAGE_SRC (message) will be the video sink element */
   overlay = GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message));
   gst_video_overlay_set_window_handle (overlay, window_handle_Ulm);
 } else {
   g_warning ("Should have obtained window_handle by now!");
 }
 gst_message_unref (message);
 return GST_BUS_DROP;
}

/* This function is called when the GUI toolkit creates the physical window that will hold the video.
 * At this point we can retrieve its handler (which has a different meaning depending on the windowing system)
 * and pass it to GStreamer through the VideoOverlay interface. */
static void realize_cb_Uschl (GtkWidget *widget, CustomData *data) {
  GdkWindow *window = gtk_widget_get_window (widget);

  if (!gdk_window_ensure_native (window))
    g_error ("Couldn't create native window needed for GstVideoOverlay!");

  /* Retrieve window handler from GDK */
#if defined (GDK_WINDOWING_WIN32)
  window_handle_Uschl = (guintptr)GDK_WINDOW_HWND (window);
#elif defined (GDK_WINDOWING_QUARTZ)
  window_handle_Uschl = gdk_quartz_window_get_nsview (window);
#elif defined (GDK_WINDOWING_X11)
  window_handle_Uschl = GDK_WINDOW_XID (window);
#endif

  /* Pass it to videoStreamUschl, which implements VideoOverlay and will forward it to the video sink */
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->waitingVideoUschl), window_handle_Uschl);
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->videoStreamUschl), window_handle_Uschl);
}

/* same function for second video window */
static void realize_cb_Ulm (GtkWidget *widget, CustomData *data) {
  GdkWindow *window = gtk_widget_get_window (widget);

  if (!gdk_window_ensure_native (window))
    g_error ("Couldn't create native window needed for GstVideoOverlay!");

  /* Retrieve window handler from GDK */
#if defined (GDK_WINDOWING_WIN32)
  window_handle_Ulm = (guintptr)GDK_WINDOW_HWND (window);
#elif defined (GDK_WINDOWING_QUARTZ)
  window_handle_Ulm = gdk_quartz_window_get_nsview (window);
#elif defined (GDK_WINDOWING_X11)
  window_handle_Ulm = GDK_WINDOW_XID (window);
#endif

  /* Pass it to videoStreamUschl, which implements VideoOverlay and will forward it to the video sink */
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->videoStreamUlm), window_handle_Ulm);
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->waitingVideoUlm), window_handle_Ulm);
}

/* This function is called when the main window is closed */
static void delete_event_cb (GtkWidget *widget, GdkEvent *event, CustomData *data) {
  /* Set all pipelines to READY state (they are set to NULL at the end of the program) */
  gst_element_set_state (data->waitingVideoUschl, GST_STATE_READY);
  gst_element_set_state (data->videoStreamUschl, GST_STATE_READY);
  gst_element_set_state (data->videoStreamUlm, GST_STATE_READY);
  gst_element_set_state (data->waitingVideoUlm, GST_STATE_READY);
}

/* This function is called everytime the video window needs to be redrawn (due to damage/exposure,
 * rescaling, etc). GStreamer takes care of this in the PAUSED and PLAYING states, otherwise,
 * we simply draw a black rectangle to avoid garbage showing up. */
static gboolean draw_cb_Uschl (GtkWidget *widget, cairo_t *cr, CustomData *data) {
  GtkAllocation allocation;
  if (data->stateStreamUschl < GST_STATE_PAUSED) {
  gtk_widget_get_allocation(widget, &allocation);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_rectangle (cr, 0, 0, 950, 1040);
    cairo_fill (cr);
  }
 return FALSE;
}

/* This function is called everytime the video window needs to be redrawn (due to damage/exposure,
 * rescaling, etc). GStreamer takes care of this in the PAUSED and PLAYING states, otherwise,
 * we simply draw a black rectangle to avoid garbage showing up. */
static gboolean draw_cb_Ulm (GtkWidget *widget, cairo_t *cr, CustomData *data) {
  GtkAllocation allocation;
  if (data->stateStreamUlm < GST_STATE_PAUSED) {
  gtk_widget_get_allocation(widget, &allocation);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_rectangle (cr, 0, 0, 950, 1040);
    cairo_fill (cr);
  }
 return FALSE;
}

/* This creates all the GTK+ widgets that compose our application, and registers the callbacks */
static void create_ui (CustomData *data) {
  GtkWidget *main_window;  /* The uppermost window, containing all other windows */
  GtkWidget *video_window_Uschl; /* The drawing area where the video will be shown */
  GtkWidget *video_window_Ulm; /* The drawing area where the video will be shown */
  GtkWidget *main_box;     /* VBox to hold main_hbox and the controls */
  GtkWidget *main_hbox;    /* HBox to hold the video_window and the stream info text widget */

  main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), data);

  video_window_Uschl = gtk_drawing_area_new();
  gtk_widget_set_double_buffered (video_window_Uschl, FALSE);
  g_signal_connect (video_window_Uschl, "realize", G_CALLBACK (realize_cb_Uschl), data);
  g_signal_connect (video_window_Uschl, "draw", G_CALLBACK (draw_cb_Uschl), data);

  video_window_Ulm = gtk_drawing_area_new();
  gtk_widget_set_double_buffered (video_window_Ulm, FALSE);
  g_signal_connect (video_window_Ulm, "realize", G_CALLBACK (realize_cb_Ulm), data);
  g_signal_connect (video_window_Ulm, "draw", G_CALLBACK (draw_cb_Ulm), data);

  main_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), video_window_Uschl, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), video_window_Ulm, TRUE, TRUE, 0);

  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start (GTK_BOX (main_box), main_hbox, TRUE, TRUE, 0);
  gtk_container_add (GTK_CONTAINER (main_window), main_box);
  gtk_window_set_default_size (GTK_WINDOW (main_window), 1920, 1080);

  gtk_widget_show_all (main_window);
}



/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) */
static void eos_cb_Uschl (GstBus *bus, GstMessage *msg, CustomData *data) {
  g_print ("End-Of-Stream reached.\n");
  /* Set the streaming pipeline to ready state, wait 1 s to make sure pipeline is ready */
  gst_element_set_state (data->videoStreamUschl, GST_STATE_READY);
  /* Set the streaming pipline to null state to dispose of complete pipeline */
  gst_element_set_state (data->videoStreamUschl, GST_STATE_NULL);
  /* Set the waiting pipeline to playing state and update state variable */
  gst_element_set_state (data->waitingVideoUschl, GST_STATE_PLAYING);
  g_timeout_add_seconds (10, (GSourceFunc) rtsp_client_Uschl, data);

}

/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) */
static void eos_cb_Ulm (GstBus *bus, GstMessage *msg, CustomData *data) {
  g_print ("End-Of-Stream2 reached.\n");
  /* Set the streaming pipeline to ready state, wait 1 s to make sure pipeline is ready */
  gst_element_set_state (data->videoStreamUlm, GST_STATE_READY);
  /* Set the streaming pipline to null state to dispose of complete pipeline */
  gst_element_set_state (data->videoStreamUlm, GST_STATE_NULL);
  /* Set the waiting pipeline to playing state and update state variable */
  gst_element_set_state (data->waitingVideoUlm, GST_STATE_PLAYING);
  g_timeout_add_seconds (10, (GSourceFunc) rtsp_client_Ulm, data);
}

/* This function is called when the pipeline changes states. We use it to
 * keep track of the current state. */
static void state_changed_cb_Uschl (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstState old_state, new_state, pending_state;
  /* Parse received message */
  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  /* check if we got a message from streaming pipeline */
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->videoStreamUschl)) {
    data->stateStreamUschl = new_state;
    g_print ("Streaming video Uschl state set to %s\n", gst_element_state_get_name (new_state));
  }
  /* When streaming pipeline is set to PLAYING state we want to immediatly make sure the waiting pipeline is set to PAUSED state */
  if (new_state == GST_STATE_PLAYING)
  {
    /* Set state of waiting pipeline to PAUSED */
   gst_element_set_state (data->waitingVideoUschl, GST_STATE_PAUSED);
  }
  /* When streaming pipeline is set to PAUSED/READY/NULL state we want to make sure the waiting pipeline is set to PLAYING state */
  if (new_state == GST_STATE_PAUSED || new_state == GST_STATE_READY || new_state == GST_STATE_NULL )
  {
    /* Set state of waiting pipeline to PLAYING */
    gst_element_set_state (data->waitingVideoUschl, GST_STATE_PLAYING);
	}
  }

/* This function is called when the pipeline changes states. We use it to
 * keep track of the current state. */
static void state_changed_cb_Ulm (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstState old_state, new_state, pending_state;
  /* Parse received message */
  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  /* check if we got a message from streaming pipeline */
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->videoStreamUlm)) {
    data->stateStreamUlm = new_state;
    g_print ("Streaming video Ulm state set to %s\n", gst_element_state_get_name (new_state));
  }
  /* When streaming pipeline is set to PLAYING state we want to immediatly make sure the waiting pipeline is set to PAUSED state */
  if (new_state == GST_STATE_PLAYING)
  {
   /* Set state of waiting pipeline to PAUSED */
   gst_element_set_state (data->waitingVideoUlm, GST_STATE_PAUSED);
  }
  /* When streaming pipeline is set to PAUSED/READY/NULL state we want to make sure the waiting pipeline is set to PLAYING state */
  if (new_state == GST_STATE_PAUSED || new_state == GST_STATE_READY || new_state == GST_STATE_NULL)
  {
   /* Set state of waiting pipeline to PLAYING */
   gst_element_set_state (data->waitingVideoUlm, GST_STATE_PLAYING);
  }
}

/* This function is called when an error message is posted on the bus */
static void error_cb_Uschl (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;
  GstStateChangeReturn ret;
  char *err_msg;
  GError *error=NULL;

  /* Set the pipeline to READY (which stops playback) */
  gst_element_set_state (data->videoStreamUschl, GST_STATE_READY);
  gst_element_set_state (data->videoStreamUschl, GST_STATE_NULL);
  /* Set pipeline 2 to playing state */
	gst_element_set_state (data->waitingVideoUschl, GST_STATE_PLAYING);
  g_timeout_add_seconds (10, (GSourceFunc) rtsp_client_Uschl, data);

  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  err_msg=err->message;
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  
  g_clear_error (&err);
  g_free (debug_info);
}


/*Same for second pipeline */
static void error_cb_Ulm (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;
  GstStateChangeReturn ret;
  char *err_msg;
  GError *error=NULL;

  /* Set the pipeline to READY (which stops playback) */
  gst_element_set_state (data->videoStreamUlm, GST_STATE_READY);
  gst_element_set_state (data->videoStreamUlm, GST_STATE_NULL);
  /* Set pipeline 2 to playing state */
  gst_element_set_state (data->waitingVideoUlm, GST_STATE_PLAYING);
  g_timeout_add_seconds (10, (GSourceFunc) rtsp_client_Ulm, data);

  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  err_msg=err->message;
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);
}

/* restart thread for first video stream */
static gboolean rtsp_client_Uschl (CustomData *data)
{
  GError *error=NULL;
  GstStateChangeReturn ret;
  gchar *pipe_desc;

  /* Create the elements */
  pipe_desc= g_strdup_printf ("rtspsrc location=rtsp://10.252.61.91:8554/test latency=0 do-retransmission=false user-id=user user-pw=password ! rtpjitterbuffer latency=10 drop-on-latency=true mode=2 ! application/x-rtp, encoding-name=H264 ! rtph264depay ! h264parse ! capsfilter caps='video/x-h264, stream-format=byte-stream, frame-rate=30/1' ! omxh264dec ! glimagesink sync=false async=false"); 
//  pipe_desc= g_strdup_printf ("rtspsrc location=rtsp://169.254.197.5:8554/test latency=0 do-retransmission=false user-id=user user-pw=password ! queue name=q ! rtpjitterbuffer latency=10 drop-on-latency=true mode=2 ! application/x-rtp, encoding-name=H264 ! rtph264depay ! h264parse ! capsfilter caps='video/x-h264, stream-format=byte-stream, frame-rate=30/1' ! omxh264dec ! glimagesink sync=false async=false"); 
  data->videoStreamUschl=gst_parse_launch(pipe_desc, &error);
  g_free(pipe_desc);

  /* Instruct the busStreamUschl to emit signals for each received message, and connect to the interesting signals */
  data->busStreamUschl = gst_element_get_bus (data->videoStreamUschl);
  gst_bus_set_sync_handler (data->busStreamUschl, (GstBusSyncHandler) bus_sync_handler_Uschl, NULL, NULL);
  gst_bus_add_signal_watch (data->busStreamUschl);
  g_signal_connect (G_OBJECT (data->busStreamUschl), "message::error", (GCallback)error_cb_Uschl, data);
  g_signal_connect (G_OBJECT (data->busStreamUschl), "message::eos", (GCallback)eos_cb_Uschl, data);
  g_signal_connect (G_OBJECT (data->busStreamUschl), "message::state-changed", (GCallback)state_changed_cb_Uschl, data);
  gst_object_unref (data->busStreamUschl);

  /* Start playing */
  ret=gst_element_set_state (data->videoStreamUschl, GST_STATE_PLAYING);
  if (ret==GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    data->videoStreamUschl=NULL;
    return TRUE;
  }
  /* return FALSE to make sure function is only called once */
  return FALSE;
}

/* restart thread for second video stream */
static gboolean rtsp_client_Ulm (CustomData *data)
{
  GError *error=NULL;
  GstStateChangeReturn ret;
  gchar *pipe_desc;

  /* Create the elements */
  pipe_desc= g_strdup_printf ("rtspsrc location=rtsp://10.252.61.135:8554/test latency=0 do-retransmission=false user-id=user user-pw=password ! rtpjitterbuffer latency=10 drop-on-latency=true mode=2 ! application/x-rtp, encoding-name=H264 ! rtph264depay ! h264parse ! capsfilter caps='video/x-h264, stream-format=byte-stream, frame-rate=30/1' ! omxh264dec ! glimagesink sync=false async=false"); 
//  pipe_desc= g_strdup_printf ("rtspsrc location=rtsp://169.254.197.5:8554/test latency=0 do-retransmission=false user-id=admin user-pw=power ! queue name=q ! rtpjitterbuffer latency=10 drop-on-latency=true mode=2 ! application/x-rtp, encoding-name=H264 ! rtph264depay ! h264parse ! capsfilter caps='video/x-h264, stream-format=byte-stream, frame-rate=30/1' ! omxh264dec ! glimagesink sync=false async=false"); 
  data->videoStreamUlm=gst_parse_launch(pipe_desc, &error);
  g_free(pipe_desc);

  /* Instruct the busStreamUlm to emit signals for each received message, and connect to the interesting signals */
  data->busStreamUlm = gst_element_get_bus (data->videoStreamUlm);
  gst_bus_set_sync_handler (data->busStreamUlm, (GstBusSyncHandler) bus_sync_handler_Ulm, NULL, NULL);
  gst_bus_add_signal_watch (data->busStreamUlm);
  g_signal_connect (G_OBJECT (data->busStreamUlm), "message::error", (GCallback)error_cb_Ulm, data);
  g_signal_connect (G_OBJECT (data->busStreamUlm), "message::eos", (GCallback)eos_cb_Ulm, data);
  g_signal_connect (G_OBJECT (data->busStreamUlm), "message::state-changed", (GCallback)state_changed_cb_Ulm, data);
  gst_object_unref (data->busStreamUlm);

  /* Start playing */
  ret=gst_element_set_state (data->videoStreamUlm, GST_STATE_PLAYING);
  if (ret==GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    data->videoStreamUlm=NULL;
    return TRUE;
  }
  /* return FALSE to make sure function is only called once */
  return FALSE;
}

/* This thread handles the startup and running of the rtsp server side */
gboolean rtsp_server(void)
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
  
  /* return FALSE to make sure function is only called once */
  return FALSE;
}

int main(int argc, char *argv[]) {
  CustomData data;
  GstStateChangeReturn ret;
  gchar *pipe_desc_Uschl;
  gchar *pipe_desc_Ulm;
  gchar *pipe_desc1;
  gchar *pipe_desc2;
  GError *error=NULL;
  GstMessage *msg;

  /* Initialize GTK */
  gtk_init (&argc, &argv);

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  /* Initialize our data structure */
  memset (&data, 0, sizeof (data));
  data.duration = GST_CLOCK_TIME_NONE;

 /* First snow pipeline (waitingVideoUschl) */
  pipe_desc_Uschl= g_strdup_printf ("multifilesrc location=/home/pi/test.h264 loop=true ! h264parse ! omxh264dec ! videocrop right=275 bottom=75 ! glimagesink sync=false async=false");
//  pipe_desc2= g_strdup_printf ("videotestsrc pattern=1 ! glimagesink ");
  data.waitingVideoUschl=gst_parse_launch(pipe_desc_Uschl, &error);
  g_free(pipe_desc_Uschl);
  error=NULL;

  /* First snow pipeline (waitingVideoUschl) */
//  pipe_desc4= g_strdup_printf ("videotestsrc pattern=1 ! glimagesink ");
  pipe_desc_Ulm= g_strdup_printf ("multifilesrc location=/home/pi/test.h264 loop=true ! h264parse ! omxh264dec ! videocrop right=275 bottom=75 ! glimagesink sync=false async=false");
  data.waitingVideoUlm=gst_parse_launch(pipe_desc_Ulm, &error);
  g_free(pipe_desc_Ulm);

  /* Create the GUI */
  create_ui (&data);

  /* Instruct the busWaitingUschl to emit signals for each received message, and connect to the interesting signals */
  data.busWaitingUschl = gst_element_get_bus (data.waitingVideoUschl);
  gst_bus_set_sync_handler (data.busWaitingUschl, (GstBusSyncHandler) bus_sync_handler_Uschl, NULL, NULL);

  gst_object_unref (data.busWaitingUschl);

  /* Start playing */
  ret=gst_element_set_state (data.waitingVideoUschl, GST_STATE_PLAYING);
  if (ret==GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (data.waitingVideoUschl);
    return -1;
  }

  /* Instruct the busStreamUlm to emit signals for each received message, and connect to the interesting signals */
  data.busWaitingUlm = gst_element_get_bus (data.waitingVideoUlm);
  gst_bus_set_sync_handler (data.busWaitingUlm, (GstBusSyncHandler) bus_sync_handler_Ulm, NULL, NULL);

  gst_object_unref (data.busWaitingUlm);

  /* Start playing */
  ret=gst_element_set_state (data.waitingVideoUlm, GST_STATE_PLAYING);
  if (ret==GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set second pipeline to the playing state.\n");
    gst_object_unref (data.waitingVideoUlm);
    return -1;
  }

  /* Register a function that GLib will call once after 10 seconds */
  g_timeout_add_seconds (10, (GSourceFunc) rtsp_client_Uschl, &data);
  /* Register a function that GLib will call once after 5 seconds */
  g_timeout_add_seconds (5, (GSourceFunc) rtsp_client_Ulm, &data);
  /* Register a function that GLib will call once after 1 second */
  //g_timeout_add_seconds (1, (GSourceFunc) rtsp_server, NULL);
  
  /* Start the GTK main loop. We will not regain control until gtk_main_quit is called. */
  gtk_main ();

  /* Free resources */
  gst_element_set_state (data.videoStreamUschl, GST_STATE_NULL);
  gst_object_unref (data.videoStreamUschl);
  gst_element_set_state (data.waitingVideoUschl, GST_STATE_NULL);
  gst_object_unref (data.waitingVideoUschl);
  gst_element_set_state (data.videoStreamUlm, GST_STATE_NULL);
  gst_object_unref (data.videoStreamUlm);
  gst_element_set_state (data.waitingVideoUlm, GST_STATE_NULL);
  gst_object_unref (data.waitingVideoUlm);
  return 0;
}
