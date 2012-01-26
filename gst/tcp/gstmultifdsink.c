/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) <2004> Thomas Vander Stichele <thomas at apestaart dot org>
 * Copyright (C) 2006 Wim Taymans <wim at fluendo dot com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-multifdsink
 * @see_also: tcpserversink
 *
 * This plugin writes incoming data to a set of file descriptors. The
 * file descriptors can be added to multifdsink by emitting the #GstMultiFdSink::add signal. 
 * For each descriptor added, the #GstMultiFdSink::client-added signal will be called.
 *
 * As of version 0.10.8, a client can also be added with the #GstMultiFdSink::add-full signal
 * that allows for more control over what and how much data a client 
 * initially receives.
 *
 * Clients can be removed from multifdsink by emitting the #GstMultiFdSink::remove signal. For
 * each descriptor removed, the #GstMultiFdSink::client-removed signal will be called. The
 * #GstMultiFdSink::client-removed signal can also be fired when multifdsink decides that a
 * client is not active anymore or, depending on the value of the
 * #GstMultiFdSink:recover-policy property, if the client is reading too slowly.
 * In all cases, multifdsink will never close a file descriptor itself.
 * The user of multifdsink is responsible for closing all file descriptors.
 * This can for example be done in response to the #GstMultiFdSink::client-fd-removed signal.
 * Note that multifdsink still has a reference to the file descriptor when the
 * #GstMultiFdSink::client-removed signal is emitted, so that "get-stats" can be performed on
 * the descriptor; it is therefore not safe to close the file descriptor in
 * the #GstMultiFdSink::client-removed signal handler, and you should use the 
 * #GstMultiFdSink::client-fd-removed signal to safely close the fd.
 *
 * Multifdsink internally keeps a queue of the incoming buffers and uses a
 * separate thread to send the buffers to the clients. This ensures that no
 * client write can block the pipeline and that clients can read with different
 * speeds.
 *
 * When adding a client to multifdsink, the #GstMultiFdSink:sync-method property will define
 * which buffer in the queued buffers will be sent first to the client. Clients 
 * can be sent the most recent buffer (which might not be decodable by the 
 * client if it is not a keyframe), the next keyframe received in 
 * multifdsink (which can take some time depending on the keyframe rate), or the
 * last received keyframe (which will cause a simple burst-on-connect). 
 * Multifdsink will always keep at least one keyframe in its internal buffers
 * when the sync-mode is set to latest-keyframe.
 *
 * As of version 0.10.8, there are additional values for the #GstMultiFdSink:sync-method 
 * property to allow finer control over burst-on-connect behaviour. By selecting
 * the 'burst' method a minimum burst size can be chosen, 'burst-keyframe'
 * additionally requires that the burst begin with a keyframe, and 
 * 'burst-with-keyframe' attempts to burst beginning with a keyframe, but will
 * prefer a minimum burst size even if it requires not starting with a keyframe.
 *
 * Multifdsink can be instructed to keep at least a minimum amount of data
 * expressed in time or byte units in its internal queues with the 
 * #GstMultiFdSink:time-min and #GstMultiFdSink:bytes-min properties respectively.
 * These properties are useful if the application adds clients with the 
 * #GstMultiFdSink::add-full signal to make sure that a burst connect can
 * actually be honored. 
 *
 * When streaming data, clients are allowed to read at a different rate than
 * the rate at which multifdsink receives data. If the client is reading too
 * fast, no data will be send to the client until multifdsink receives more
 * data. If the client, however, reads too slowly, data for that client will be 
 * queued up in multifdsink. Two properties control the amount of data 
 * (buffers) that is queued in multifdsink: #GstMultiFdSink:buffers-max and 
 * #GstMultiFdSink:buffers-soft-max. A client that falls behind by
 * #GstMultiFdSink:buffers-max is removed from multifdsink forcibly.
 *
 * A client with a lag of at least #GstMultiFdSink:buffers-soft-max enters the recovery
 * procedure which is controlled with the #GstMultiFdSink:recover-policy property.
 * A recover policy of NONE will do nothing, RESYNC_LATEST will send the most recently
 * received buffer as the next buffer for the client, RESYNC_SOFT_LIMIT
 * positions the client to the soft limit in the buffer queue and
 * RESYNC_KEYFRAME positions the client at the most recent keyframe in the
 * buffer queue.
 *
 * multifdsink will by default synchronize on the clock before serving the 
 * buffers to the clients. This behaviour can be disabled by setting the sync 
 * property to FALSE. Multifdsink will by default not do QoS and will never
 * drop late buffers.
 *
 * Last reviewed on 2006-09-12 (0.10.10)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst-i18n-plugin.h>

#include <sys/ioctl.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>

#ifdef HAVE_FIONREAD_IN_SYS_FILIO
#include <sys/filio.h>
#endif

#include "gstmultifdsink.h"
#include "gsttcp-marshal.h"

#define NOT_IMPLEMENTED 0

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

GST_DEBUG_CATEGORY_STATIC (multifdsink_debug);
#define GST_CAT_DEFAULT (multifdsink_debug)

/* MultiFdSink signals and args */
enum
{
  /* methods */
  SIGNAL_ADD,
  SIGNAL_ADD_BURST,
  SIGNAL_REMOVE,
  SIGNAL_REMOVE_FLUSH,
  SIGNAL_GET_STATS,

  /* signals */
  SIGNAL_CLIENT_ADDED,
  SIGNAL_CLIENT_REMOVED,
  SIGNAL_CLIENT_FD_REMOVED,

  LAST_SIGNAL
};


/* this is really arbitrarily chosen */
#define DEFAULT_MODE                    1
#define DEFAULT_BUFFERS_MAX             -1
#define DEFAULT_BUFFERS_SOFT_MAX        -1
#define DEFAULT_UNIT_TYPE               GST_TCP_UNIT_TYPE_BUFFERS
#define DEFAULT_UNITS_MAX               -1
#define DEFAULT_UNITS_SOFT_MAX          -1

#define DEFAULT_BURST_UNIT              GST_TCP_UNIT_TYPE_UNDEFINED
#define DEFAULT_BURST_VALUE             0

#define DEFAULT_QOS_DSCP                -1
#define DEFAULT_HANDLE_READ             TRUE

enum
{
  PROP_0,
  PROP_MODE,

  PROP_UNIT_TYPE,
  PROP_UNITS_MAX,
  PROP_UNITS_SOFT_MAX,

  PROP_BUFFERS_MAX,
  PROP_BUFFERS_SOFT_MAX,

  PROP_BURST_UNIT,
  PROP_BURST_VALUE,

  PROP_QOS_DSCP,

  PROP_HANDLE_READ,

  PROP_NUM_FDS,

  PROP_LAST
};

/* For backward compat, we can't really select the poll mode anymore with
 * GstPoll. */
#define GST_TYPE_FDSET_MODE (gst_fdset_mode_get_type())
static GType
gst_fdset_mode_get_type (void)
{
  static GType fdset_mode_type = 0;
  static const GEnumValue fdset_mode[] = {
    {0, "Select", "select"},
    {1, "Poll", "poll"},
    {2, "EPoll", "epoll"},
    {0, NULL, NULL},
  };

  if (!fdset_mode_type) {
    fdset_mode_type = g_enum_register_static ("GstFDSetMode", fdset_mode);
  }
  return fdset_mode_type;
}

#define GST_TYPE_UNIT_TYPE (gst_unit_type_get_type())
static GType
gst_unit_type_get_type (void)
{
  static GType unit_type_type = 0;
  static const GEnumValue unit_type[] = {
    {GST_TCP_UNIT_TYPE_UNDEFINED, "Undefined", "undefined"},
    {GST_TCP_UNIT_TYPE_BUFFERS, "Buffers", "buffers"},
    {GST_TCP_UNIT_TYPE_BYTES, "Bytes", "bytes"},
    {GST_TCP_UNIT_TYPE_TIME, "Time", "time"},
    {0, NULL, NULL},
  };

  if (!unit_type_type) {
    unit_type_type = g_enum_register_static ("GstTCPUnitType", unit_type);
  }
  return unit_type_type;
}

static void gst_multi_fd_sink_finalize (GObject * object);

static void gst_multi_fd_sink_clear_post (GstMultiHandleSink * mhsink);
static void gst_multi_fd_sink_stop_pre (GstMultiHandleSink * mhsink);
static void gst_multi_fd_sink_stop_post (GstMultiHandleSink * mhsink);
static gboolean gst_multi_fd_sink_start_pre (GstMultiHandleSink * mhsink);
static gpointer gst_multi_fd_sink_thread (GstMultiHandleSink * mhsink);

static void gst_multi_fd_sink_remove_client_link (GstMultiHandleSink * sink,
    GList * link);

static GstFlowReturn gst_multi_fd_sink_render (GstBaseSink * bsink,
    GstBuffer * buf);

static void gst_multi_fd_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_multi_fd_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

#define gst_multi_fd_sink_parent_class parent_class
G_DEFINE_TYPE (GstMultiFdSink, gst_multi_fd_sink, GST_TYPE_MULTI_HANDLE_SINK);

static guint gst_multi_fd_sink_signals[LAST_SIGNAL] = { 0 };

static void
gst_multi_fd_sink_class_init (GstMultiFdSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;
  GstMultiHandleSinkClass *gstmultihandlesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  gstmultihandlesink_class = (GstMultiHandleSinkClass *) klass;

  gobject_class->set_property = gst_multi_fd_sink_set_property;
  gobject_class->get_property = gst_multi_fd_sink_get_property;
  gobject_class->finalize = gst_multi_fd_sink_finalize;

  /**
   * GstMultiFdSink::mode
   *
   * The mode for selecting activity on the fds. 
   *
   * This property is deprecated since 0.10.18, if will now automatically
   * select and use the most optimal method.
   */
  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_enum ("mode", "Mode",
          "The mode for selecting activity on the fds (deprecated)",
          GST_TYPE_FDSET_MODE, DEFAULT_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BUFFERS_MAX,
      g_param_spec_int ("buffers-max", "Buffers max",
          "max number of buffers to queue for a client (-1 = no limit)", -1,
          G_MAXINT, DEFAULT_BUFFERS_MAX,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BUFFERS_SOFT_MAX,
      g_param_spec_int ("buffers-soft-max", "Buffers soft max",
          "Recover client when going over this limit (-1 = no limit)", -1,
          G_MAXINT, DEFAULT_BUFFERS_SOFT_MAX,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_UNIT_TYPE,
      g_param_spec_enum ("unit-type", "Units type",
          "The unit to measure the max/soft-max/queued properties",
          GST_TYPE_UNIT_TYPE, DEFAULT_UNIT_TYPE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_UNITS_MAX,
      g_param_spec_int64 ("units-max", "Units max",
          "max number of units to queue (-1 = no limit)", -1, G_MAXINT64,
          DEFAULT_UNITS_MAX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_UNITS_SOFT_MAX,
      g_param_spec_int64 ("units-soft-max", "Units soft max",
          "Recover client when going over this limit (-1 = no limit)", -1,
          G_MAXINT64, DEFAULT_UNITS_SOFT_MAX,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BURST_UNIT,
      g_param_spec_enum ("burst-unit", "Burst unit",
          "The format of the burst units (when sync-method is burst[[-with]-keyframe])",
          GST_TYPE_UNIT_TYPE, DEFAULT_BURST_UNIT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BURST_VALUE,
      g_param_spec_uint64 ("burst-value", "Burst value",
          "The amount of burst expressed in burst-unit", 0, G_MAXUINT64,
          DEFAULT_BURST_VALUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_QOS_DSCP,
      g_param_spec_int ("qos-dscp", "QoS diff srv code point",
          "Quality of Service, differentiated services code point (-1 default)",
          -1, 63, DEFAULT_QOS_DSCP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstMultiFdSink::handle-read
   *
   * Handle read requests from clients and discard the data.
   *
   * Since: 0.10.23
   */
  g_object_class_install_property (gobject_class, PROP_HANDLE_READ,
      g_param_spec_boolean ("handle-read", "Handle Read",
          "Handle client reads and discard the data",
          DEFAULT_HANDLE_READ, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_FDS,
      g_param_spec_uint ("num-fds", "Number of fds",
          "The current number of client file descriptors.",
          0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * GstMultiFdSink::add:
   * @gstmultifdsink: the multifdsink element to emit this signal on
   * @fd:             the file descriptor to add to multifdsink
   *
   * Hand the given open file descriptor to multifdsink to write to.
   */
  gst_multi_fd_sink_signals[SIGNAL_ADD] =
      g_signal_new ("add", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, G_STRUCT_OFFSET (GstMultiFdSinkClass,
          add), NULL, NULL, g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1,
      G_TYPE_INT);
  /**
   * GstMultiFdSink::add-full:
   * @gstmultifdsink: the multifdsink element to emit this signal on
   * @fd:             the file descriptor to add to multifdsink
   * @sync:           the sync method to use
   * @unit_type_min:  the unit-type of @value_min
   * @value_min:      the minimum amount of data to burst expressed in
   *                  @unit_type_min units.
   * @unit_type_max:  the unit-type of @value_max
   * @value_max:      the maximum amount of data to burst expressed in
   *                  @unit_type_max units.
   *
   * Hand the given open file descriptor to multifdsink to write to and
   * specify the burst parameters for the new connection.
   */
  gst_multi_fd_sink_signals[SIGNAL_ADD_BURST] =
      g_signal_new ("add-full", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, G_STRUCT_OFFSET (GstMultiFdSinkClass,
          add_full), NULL, NULL,
      gst_tcp_marshal_VOID__INT_ENUM_INT_UINT64_INT_UINT64,
      G_TYPE_NONE, 6, G_TYPE_INT, GST_TYPE_SYNC_METHOD, GST_TYPE_UNIT_TYPE,
      G_TYPE_UINT64, GST_TYPE_UNIT_TYPE, G_TYPE_UINT64);
  /**
   * GstMultiFdSink::remove:
   * @gstmultifdsink: the multifdsink element to emit this signal on
   * @fd:             the file descriptor to remove from multifdsink
   *
   * Remove the given open file descriptor from multifdsink.
   */
  gst_multi_fd_sink_signals[SIGNAL_REMOVE] =
      g_signal_new ("remove", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, G_STRUCT_OFFSET (GstMultiFdSinkClass,
          remove), NULL, NULL, gst_tcp_marshal_VOID__INT, G_TYPE_NONE,
      1, G_TYPE_INT);
  /**
   * GstMultiFdSink::remove-flush:
   * @gstmultifdsink: the multifdsink element to emit this signal on
   * @fd:             the file descriptor to remove from multifdsink
   *
   * Remove the given open file descriptor from multifdsink after flushing all
   * the pending data to the fd.
   */
  gst_multi_fd_sink_signals[SIGNAL_REMOVE_FLUSH] =
      g_signal_new ("remove-flush", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, G_STRUCT_OFFSET (GstMultiFdSinkClass,
          remove_flush), NULL, NULL, gst_tcp_marshal_VOID__INT,
      G_TYPE_NONE, 1, G_TYPE_INT);

  /**
   * GstMultiFdSink::get-stats:
   * @gstmultifdsink: the multifdsink element to emit this signal on
   * @fd:             the file descriptor to get stats of from multifdsink
   *
   * Get statistics about @fd. This function returns a GValueArray to ease
   * automatic wrapping for bindings.
   *
   * Returns: a GValueArray with the statistics. The array contains guint64
   *     values that represent respectively: total number of bytes sent, time
   *     when the client was added, time when the client was
   *     disconnected/removed, time the client is/was active, last activity
   *     time (in epoch seconds), number of buffers dropped.
   *     All times are expressed in nanoseconds (GstClockTime).
   *     The array can be 0-length if the client was not found.
   */
  gst_multi_fd_sink_signals[SIGNAL_GET_STATS] =
      g_signal_new ("get-stats", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, G_STRUCT_OFFSET (GstMultiFdSinkClass,
          get_stats), NULL, NULL, gst_tcp_marshal_BOXED__INT,
      G_TYPE_VALUE_ARRAY, 1, G_TYPE_INT);

  /**
   * GstMultiFdSink::client-added:
   * @gstmultifdsink: the multifdsink element that emitted this signal
   * @fd:             the file descriptor that was added to multifdsink
   *
   * The given file descriptor was added to multifdsink. This signal will
   * be emitted from the streaming thread so application should be prepared
   * for that.
   */
  gst_multi_fd_sink_signals[SIGNAL_CLIENT_ADDED] =
      g_signal_new ("client-added", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstMultiFdSinkClass, client_added),
      NULL, NULL, gst_tcp_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
  /**
   * GstMultiFdSink::client-removed:
   * @gstmultifdsink: the multifdsink element that emitted this signal
   * @fd:             the file descriptor that is to be removed from multifdsink
   * @status:         the reason why the client was removed
   *
   * The given file descriptor is about to be removed from multifdsink. This
   * signal will be emitted from the streaming thread so applications should
   * be prepared for that.
   *
   * @gstmultifdsink still holds a handle to @fd so it is possible to call
   * the get-stats signal from this callback. For the same reason it is
   * not safe to close() and reuse @fd in this callback.
   */
  gst_multi_fd_sink_signals[SIGNAL_CLIENT_REMOVED] =
      g_signal_new ("client-removed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstMultiFdSinkClass,
          client_removed), NULL, NULL, gst_tcp_marshal_VOID__INT_ENUM,
      G_TYPE_NONE, 2, G_TYPE_INT, GST_TYPE_CLIENT_STATUS);
  /**
   * GstMultiFdSink::client-fd-removed:
   * @gstmultifdsink: the multifdsink element that emitted this signal
   * @fd:             the file descriptor that was removed from multifdsink
   *
   * The given file descriptor was removed from multifdsink. This signal will
   * be emitted from the streaming thread so applications should be prepared
   * for that.
   *
   * In this callback, @gstmultifdsink has removed all the information
   * associated with @fd and it is therefore not possible to call get-stats
   * with @fd. It is however safe to close() and reuse @fd in the callback.
   *
   * Since: 0.10.7
   */
  gst_multi_fd_sink_signals[SIGNAL_CLIENT_FD_REMOVED] =
      g_signal_new ("client-fd-removed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstMultiFdSinkClass,
          client_fd_removed), NULL, NULL, gst_tcp_marshal_VOID__INT,
      G_TYPE_NONE, 1, G_TYPE_INT);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sinktemplate));

  gst_element_class_set_details_simple (gstelement_class,
      "Multi filedescriptor sink", "Sink/Network",
      "Send data to multiple filedescriptors",
      "Thomas Vander Stichele <thomas at apestaart dot org>, "
      "Wim Taymans <wim@fluendo.com>");

  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_multi_fd_sink_render);

  gstmultihandlesink_class->clear_post =
      GST_DEBUG_FUNCPTR (gst_multi_fd_sink_clear_post);

  gstmultihandlesink_class->stop_pre =
      GST_DEBUG_FUNCPTR (gst_multi_fd_sink_stop_pre);
  gstmultihandlesink_class->stop_post =
      GST_DEBUG_FUNCPTR (gst_multi_fd_sink_stop_post);
  gstmultihandlesink_class->start_pre =
      GST_DEBUG_FUNCPTR (gst_multi_fd_sink_start_pre);
  gstmultihandlesink_class->thread =
      GST_DEBUG_FUNCPTR (gst_multi_fd_sink_thread);

  gstmultihandlesink_class->remove_client_link =
      GST_DEBUG_FUNCPTR (gst_multi_fd_sink_remove_client_link);


  klass->add = GST_DEBUG_FUNCPTR (gst_multi_fd_sink_add);
  klass->add_full = GST_DEBUG_FUNCPTR (gst_multi_fd_sink_add_full);
  klass->remove = GST_DEBUG_FUNCPTR (gst_multi_fd_sink_remove);
  klass->remove_flush = GST_DEBUG_FUNCPTR (gst_multi_fd_sink_remove_flush);
  klass->get_stats = GST_DEBUG_FUNCPTR (gst_multi_fd_sink_get_stats);

  GST_DEBUG_CATEGORY_INIT (multifdsink_debug, "multifdsink", 0, "FD sink");
}

static void
gst_multi_fd_sink_init (GstMultiFdSink * this)
{
  this->mode = DEFAULT_MODE;

  this->fd_hash = g_hash_table_new (g_int_hash, g_int_equal);

  this->unit_type = DEFAULT_UNIT_TYPE;
  this->units_max = DEFAULT_UNITS_MAX;
  this->units_soft_max = DEFAULT_UNITS_SOFT_MAX;

  this->def_burst_unit = DEFAULT_BURST_UNIT;
  this->def_burst_value = DEFAULT_BURST_VALUE;

  this->qos_dscp = DEFAULT_QOS_DSCP;
  this->handle_read = DEFAULT_HANDLE_READ;

  this->header_flags = 0;
}

static void
gst_multi_fd_sink_finalize (GObject * object)
{
  GstMultiFdSink *this = GST_MULTI_FD_SINK (object);

  g_hash_table_destroy (this->fd_hash);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gint
setup_dscp_client (GstMultiFdSink * sink, GstTCPClient * client)
{
  gint tos;
  gint ret;
  union gst_sockaddr
  {
    struct sockaddr sa;
    struct sockaddr_in6 sa_in6;
    struct sockaddr_storage sa_stor;
  } sa;
  socklen_t slen = sizeof (sa);
  gint af;

  /* don't touch */
  if (sink->qos_dscp < 0)
    return 0;

  if ((ret = getsockname (client->fd.fd, &sa.sa, &slen)) < 0) {
    GST_DEBUG_OBJECT (sink, "could not get sockname: %s", g_strerror (errno));
    return ret;
  }

  af = sa.sa.sa_family;

  /* if this is an IPv4-mapped address then do IPv4 QoS */
  if (af == AF_INET6) {

    GST_DEBUG_OBJECT (sink, "check IP6 socket");
    if (IN6_IS_ADDR_V4MAPPED (&(sa.sa_in6.sin6_addr))) {
      GST_DEBUG_OBJECT (sink, "mapped to IPV4");
      af = AF_INET;
    }
  }

  /* extract and shift 6 bits of the DSCP */
  tos = (sink->qos_dscp & 0x3f) << 2;

  switch (af) {
    case AF_INET:
      ret = setsockopt (client->fd.fd, IPPROTO_IP, IP_TOS, &tos, sizeof (tos));
      break;
    case AF_INET6:
#ifdef IPV6_TCLASS
      ret =
          setsockopt (client->fd.fd, IPPROTO_IPV6, IPV6_TCLASS, &tos,
          sizeof (tos));
      break;
#endif
    default:
      ret = 0;
      GST_ERROR_OBJECT (sink, "unsupported AF");
      break;
  }
  if (ret)
    GST_DEBUG_OBJECT (sink, "could not set DSCP: %s", g_strerror (errno));

  return ret;
}


static void
setup_dscp (GstMultiFdSink * sink)
{
  GList *clients, *next;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);

  CLIENTS_LOCK (sink);
  for (clients = mhsink->clients; clients; clients = next) {
    GstTCPClient *client;

    client = (GstTCPClient *) clients->data;
    next = g_list_next (clients);

    setup_dscp_client (sink, client);
  }
  CLIENTS_UNLOCK (sink);
}

/* "add-full" signal implementation */
void
gst_multi_fd_sink_add_full (GstMultiFdSink * sink, int fd,
    GstSyncMethod sync_method, GstTCPUnitType min_unit, guint64 min_value,
    GstTCPUnitType max_unit, guint64 max_value)
{
  GstTCPClient *client;
  GstMultiHandleClient *mhclient;
  GList *clink;
  gint flags;
  struct stat statbuf;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);

  GST_DEBUG_OBJECT (sink, "[fd %5d] adding client, sync_method %d, "
      "min_unit %d, min_value %" G_GUINT64_FORMAT
      ", max_unit %d, max_value %" G_GUINT64_FORMAT, fd, sync_method,
      min_unit, min_value, max_unit, max_value);

  /* do limits check if we can */
  if (min_unit == max_unit) {
    if (max_value != -1 && min_value != -1 && max_value < min_value)
      goto wrong_limits;
  }

  /* create client datastructure */
  client = g_new0 (GstTCPClient, 1);
  mhclient = (GstMultiHandleClient *) client;
  gst_multi_handle_sink_client_init (mhclient, sync_method);
  g_snprintf (mhclient->debug, 30, "[fd %5d]", fd);

  client->fd.fd = fd;
  client->burst_min_unit = min_unit;
  client->burst_min_value = min_value;
  client->burst_max_unit = max_unit;
  client->burst_max_value = max_value;

  CLIENTS_LOCK (sink);

  /* check the hash to find a duplicate fd */
  clink = g_hash_table_lookup (sink->fd_hash, &client->fd.fd);
  if (clink != NULL)
    goto duplicate;

  /* we can add the fd now */
  clink = mhsink->clients = g_list_prepend (mhsink->clients, client);
  g_hash_table_insert (sink->fd_hash, &client->fd.fd, clink);
  mhsink->clients_cookie++;

  /* set the socket to non blocking */
  if (fcntl (fd, F_SETFL, O_NONBLOCK) < 0) {
    GST_ERROR_OBJECT (sink, "failed to make socket %d non-blocking: %s", fd,
        g_strerror (errno));
  }

  /* we always read from a client */
  gst_poll_add_fd (sink->fdset, &client->fd);

  /* we don't try to read from write only fds */
  if (sink->handle_read) {
    flags = fcntl (fd, F_GETFL, 0);
    if ((flags & O_ACCMODE) != O_WRONLY) {
      gst_poll_fd_ctl_read (sink->fdset, &client->fd, TRUE);
    }
  }
  /* figure out the mode, can't use send() for non sockets */
  if (fstat (fd, &statbuf) == 0 && S_ISSOCK (statbuf.st_mode)) {
    client->is_socket = TRUE;
    setup_dscp_client (sink, client);
  }

  gst_poll_restart (sink->fdset);

  CLIENTS_UNLOCK (sink);

  g_signal_emit (G_OBJECT (sink),
      gst_multi_fd_sink_signals[SIGNAL_CLIENT_ADDED], 0, fd);

  return;

  /* errors */
wrong_limits:
  {
    GST_WARNING_OBJECT (sink,
        "[fd %5d] wrong values min =%" G_GUINT64_FORMAT ", max=%"
        G_GUINT64_FORMAT ", unit %d specified when adding client", fd,
        min_value, max_value, min_unit);
    return;
  }
duplicate:
  {
    mhclient->status = GST_CLIENT_STATUS_DUPLICATE;
    CLIENTS_UNLOCK (sink);
    GST_WARNING_OBJECT (sink, "[fd %5d] duplicate client found, refusing", fd);
    g_signal_emit (G_OBJECT (sink),
        gst_multi_fd_sink_signals[SIGNAL_CLIENT_REMOVED], 0, fd,
        mhclient->status);
    g_free (client);
    return;
  }
}

/* "add" signal implementation */
void
gst_multi_fd_sink_add (GstMultiFdSink * sink, int fd)
{
  GstMultiHandleSink *mhsink;

  mhsink = GST_MULTI_HANDLE_SINK (sink);
  gst_multi_fd_sink_add_full (sink, fd, mhsink->def_sync_method,
      sink->def_burst_unit, sink->def_burst_value, sink->def_burst_unit, -1);
}

/* "remove" signal implementation */
void
gst_multi_fd_sink_remove (GstMultiFdSink * sink, int fd)
{
  GList *clink;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);
  GstMultiHandleSinkClass *mhsinkclass =
      GST_MULTI_HANDLE_SINK_GET_CLASS (mhsink);

  GST_DEBUG_OBJECT (sink, "[fd %5d] removing client", fd);

  CLIENTS_LOCK (sink);
  clink = g_hash_table_lookup (sink->fd_hash, &fd);
  if (clink != NULL) {
    GstTCPClient *client = (GstTCPClient *) clink->data;
    GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;

    if (mhclient->status != GST_CLIENT_STATUS_OK) {
      GST_INFO_OBJECT (sink,
          "[fd %5d] Client already disconnecting with status %d",
          fd, mhclient->status);
      goto done;
    }

    mhclient->status = GST_CLIENT_STATUS_REMOVED;
    mhsinkclass->remove_client_link (GST_MULTI_HANDLE_SINK (sink), clink);
    // FIXME: specific poll
    gst_poll_restart (sink->fdset);
  } else {
    GST_WARNING_OBJECT (sink, "[fd %5d] no client with this fd found!", fd);
  }

done:
  CLIENTS_UNLOCK (sink);
}

/* "remove-flush" signal implementation */
void
gst_multi_fd_sink_remove_flush (GstMultiFdSink * sink, int fd)
{
  GList *clink;

  GST_DEBUG_OBJECT (sink, "[fd %5d] flushing client", fd);

  CLIENTS_LOCK (sink);
  clink = g_hash_table_lookup (sink->fd_hash, &fd);
  if (clink != NULL) {
    GstTCPClient *client = (GstTCPClient *) clink->data;
    GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;

    if (mhclient->status != GST_CLIENT_STATUS_OK) {
      GST_INFO_OBJECT (sink,
          "[fd %5d] Client already disconnecting with status %d",
          fd, mhclient->status);
      goto done;
    }

    /* take the position of the client as the number of buffers left to flush.
     * If the client was at position -1, we flush 0 buffers, 0 == flush 1
     * buffer, etc... */
    mhclient->flushcount = mhclient->bufpos + 1;
    /* mark client as flushing. We can not remove the client right away because
     * it might have some buffers to flush in the ->sending queue. */
    mhclient->status = GST_CLIENT_STATUS_FLUSHING;
  } else {
    GST_WARNING_OBJECT (sink, "[fd %5d] no client with this fd found!", fd);
  }
done:
  CLIENTS_UNLOCK (sink);
}

/* called with the CLIENTS_LOCK held */
static void
gst_multi_fd_sink_clear_post (GstMultiHandleSink * mhsink)
{
  GstMultiFdSink *sink = GST_MULTI_FD_SINK (mhsink);

  gst_poll_restart (sink->fdset);
}

/* "get-stats" signal implementation
 * the array returned contains:
 *
 * guint64 : bytes_sent
 * guint64 : connect time (in nanoseconds, since Epoch)
 * guint64 : disconnect time (in nanoseconds, since Epoch)
 * guint64 : time the client is/was connected (in nanoseconds)
 * guint64 : last activity time (in nanoseconds, since Epoch)
 * guint64 : buffers dropped due to recovery
 * guint64 : timestamp of the first buffer sent (in nanoseconds)
 * guint64 : timestamp of the last buffer sent (in nanoseconds)
 */
GValueArray *
gst_multi_fd_sink_get_stats (GstMultiFdSink * sink, int fd)
{
  GstTCPClient *client;
  GValueArray *result = NULL;
  GList *clink;

  CLIENTS_LOCK (sink);
  clink = g_hash_table_lookup (sink->fd_hash, &fd);
  if (clink == NULL)
    goto noclient;

  client = (GstTCPClient *) clink->data;
  if (client != NULL) {
    GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;
    GValue value = { 0 };
    guint64 interval;

    result = g_value_array_new (7);

    g_value_init (&value, G_TYPE_UINT64);
    g_value_set_uint64 (&value, mhclient->bytes_sent);
    result = g_value_array_append (result, &value);
    g_value_unset (&value);
    g_value_init (&value, G_TYPE_UINT64);
    g_value_set_uint64 (&value, mhclient->connect_time);
    result = g_value_array_append (result, &value);
    g_value_unset (&value);
    if (mhclient->disconnect_time == 0) {
      GTimeVal nowtv;

      g_get_current_time (&nowtv);

      interval = GST_TIMEVAL_TO_TIME (nowtv) - mhclient->connect_time;
    } else {
      interval = mhclient->disconnect_time - mhclient->connect_time;
    }
    g_value_init (&value, G_TYPE_UINT64);
    g_value_set_uint64 (&value, mhclient->disconnect_time);
    result = g_value_array_append (result, &value);
    g_value_unset (&value);
    g_value_init (&value, G_TYPE_UINT64);
    g_value_set_uint64 (&value, interval);
    result = g_value_array_append (result, &value);
    g_value_unset (&value);
    g_value_init (&value, G_TYPE_UINT64);
    g_value_set_uint64 (&value, mhclient->last_activity_time);
    result = g_value_array_append (result, &value);
    g_value_unset (&value);
    g_value_init (&value, G_TYPE_UINT64);
    g_value_set_uint64 (&value, mhclient->dropped_buffers);
    result = g_value_array_append (result, &value);
    g_value_unset (&value);
    g_value_init (&value, G_TYPE_UINT64);
    g_value_set_uint64 (&value, mhclient->first_buffer_ts);
    result = g_value_array_append (result, &value);
    g_value_unset (&value);
    g_value_init (&value, G_TYPE_UINT64);
    g_value_set_uint64 (&value, mhclient->last_buffer_ts);
    result = g_value_array_append (result, &value);
  }

noclient:
  CLIENTS_UNLOCK (sink);

  /* python doesn't like a NULL pointer yet */
  if (result == NULL) {
    GST_WARNING_OBJECT (sink, "[fd %5d] no client with this found!", fd);
    result = g_value_array_new (0);
  }

  return result;
}

/* should be called with the clientslock helt.
 * Note that we don't close the fd as we didn't open it in the first
 * place. An application should connect to the client-fd-removed signal and
 * close the fd itself.
 */
static void
gst_multi_fd_sink_remove_client_link (GstMultiHandleSink * sink, GList * link)
{
  int fd;
  GTimeVal now;
  GstTCPClient *client = (GstTCPClient *) link->data;
  GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;
  GstMultiFdSink *mfsink = GST_MULTI_FD_SINK (sink);
  GstMultiFdSinkClass *fclass;

  fclass = GST_MULTI_FD_SINK_GET_CLASS (sink);

  fd = client->fd.fd;

  if (mhclient->currently_removing) {
    GST_WARNING_OBJECT (sink, "%s client is already being removed",
        mhclient->debug);
    return;
  } else {
    mhclient->currently_removing = TRUE;
  }

  /* FIXME: if we keep track of ip we can log it here and signal */
  switch (mhclient->status) {
    case GST_CLIENT_STATUS_OK:
      GST_WARNING_OBJECT (sink, "%s removing client %p for no reason",
          mhclient->debug, client);
      break;
    case GST_CLIENT_STATUS_CLOSED:
      GST_DEBUG_OBJECT (sink, "%s removing client %p because of close",
          mhclient->debug, client);
      break;
    case GST_CLIENT_STATUS_REMOVED:
      GST_DEBUG_OBJECT (sink,
          "%s removing client %p because the app removed it", mhclient->debug,
          client);
      break;
    case GST_CLIENT_STATUS_SLOW:
      GST_INFO_OBJECT (sink,
          "%s removing client %p because it was too slow", mhclient->debug,
          client);
      break;
    case GST_CLIENT_STATUS_ERROR:
      GST_WARNING_OBJECT (sink,
          "%s removing client %p because of error", mhclient->debug, client);
      break;
    case GST_CLIENT_STATUS_FLUSHING:
    default:
      GST_WARNING_OBJECT (sink,
          "%s removing client %p with invalid reason %d", mhclient->debug,
          client, mhclient->status);
      break;
  }

  gst_poll_remove_fd (mfsink->fdset, &client->fd);

  g_get_current_time (&now);
  mhclient->disconnect_time = GST_TIMEVAL_TO_TIME (now);

  /* free client buffers */
  g_slist_foreach (mhclient->sending, (GFunc) gst_mini_object_unref, NULL);
  g_slist_free (mhclient->sending);
  mhclient->sending = NULL;

  if (mhclient->caps)
    gst_caps_unref (mhclient->caps);
  mhclient->caps = NULL;

  /* unlock the mutex before signaling because the signal handler
   * might query some properties */
  CLIENTS_UNLOCK (sink);

  g_signal_emit (G_OBJECT (sink),
      gst_multi_fd_sink_signals[SIGNAL_CLIENT_REMOVED], 0, fd,
      mhclient->status);

  /* lock again before we remove the client completely */
  CLIENTS_LOCK (sink);

  /* fd cannot be reused in the above signal callback so we can safely
   * remove it from the hashtable here */
  if (!g_hash_table_remove (mfsink->fd_hash, &client->fd.fd)) {
    GST_WARNING_OBJECT (sink,
        "[fd %5d] error removing client %p from hash", client->fd.fd, client);
  }
  /* after releasing the lock above, the link could be invalid, more
   * precisely, the next and prev pointers could point to invalid list
   * links. One optimisation could be to add a cookie to the linked list
   * and take a shortcut when it did not change between unlocking and locking
   * our mutex. For now we just walk the list again. */
  sink->clients = g_list_remove (sink->clients, client);
  sink->clients_cookie++;

  if (fclass->removed)
    fclass->removed (mfsink, client->fd.fd);

  g_free (client);
  CLIENTS_UNLOCK (sink);

  /* and the fd is really gone now */
  g_signal_emit (G_OBJECT (sink),
      gst_multi_fd_sink_signals[SIGNAL_CLIENT_FD_REMOVED], 0, fd);

  CLIENTS_LOCK (sink);
}

/* handle a read on a client fd,
 * which either indicates a close or should be ignored
 * returns FALSE if some error occured or the client closed. */
static gboolean
gst_multi_fd_sink_handle_client_read (GstMultiFdSink * sink,
    GstTCPClient * client)
{
  int avail, fd;
  gboolean ret;
  GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;

  fd = client->fd.fd;

  if (ioctl (fd, FIONREAD, &avail) < 0)
    goto ioctl_failed;

  GST_DEBUG_OBJECT (sink, "[fd %5d] select reports client read of %d bytes",
      fd, avail);

  ret = TRUE;

  if (avail == 0) {
    /* client sent close, so remove it */
    GST_DEBUG_OBJECT (sink, "[fd %5d] client asked for close, removing", fd);
    mhclient->status = GST_CLIENT_STATUS_CLOSED;
    ret = FALSE;
  } else if (avail < 0) {
    GST_WARNING_OBJECT (sink, "[fd %5d] avail < 0, removing", fd);
    mhclient->status = GST_CLIENT_STATUS_ERROR;
    ret = FALSE;
  } else {
    guint8 dummy[512];
    gint nread;

    /* just Read 'n' Drop, could also just drop the client as it's not supposed
     * to write to us except for closing the socket, I guess it's because we
     * like to listen to our customers. */
    do {
      /* this is the maximum we can read */
      gint to_read = MIN (avail, 512);

      GST_DEBUG_OBJECT (sink, "[fd %5d] client wants us to read %d bytes",
          fd, to_read);

      nread = read (fd, dummy, to_read);
      if (nread < -1) {
        GST_WARNING_OBJECT (sink, "[fd %5d] could not read %d bytes: %s (%d)",
            fd, to_read, g_strerror (errno), errno);
        mhclient->status = GST_CLIENT_STATUS_ERROR;
        ret = FALSE;
        break;
      } else if (nread == 0) {
        GST_WARNING_OBJECT (sink, "[fd %5d] 0 bytes in read, removing", fd);
        mhclient->status = GST_CLIENT_STATUS_ERROR;
        ret = FALSE;
        break;
      }
      avail -= nread;
    }
    while (avail > 0);
  }
  return ret;

  /* ERRORS */
ioctl_failed:
  {
    GST_WARNING_OBJECT (sink, "[fd %5d] ioctl failed: %s (%d)",
        fd, g_strerror (errno), errno);
    mhclient->status = GST_CLIENT_STATUS_ERROR;
    return FALSE;
  }
}

/* queue the given buffer for the given client */
static gboolean
gst_multi_fd_sink_client_queue_buffer (GstMultiFdSink * sink,
    GstTCPClient * client, GstBuffer * buffer)
{
  GstCaps *caps;

  /* TRUE: send them if the new caps have them */
  gboolean send_streamheader = FALSE;
  GstStructure *s;
  GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);

  /* before we queue the buffer, we check if we need to queue streamheader
   * buffers (because it's a new client, or because they changed) */
  caps = gst_pad_get_current_caps (GST_BASE_SINK_PAD (sink));

  if (!mhclient->caps) {
    GST_DEBUG_OBJECT (sink,
        "[fd %5d] no previous caps for this client, send streamheader",
        client->fd.fd);
    send_streamheader = TRUE;
    mhclient->caps = gst_caps_ref (caps);
  } else {
    /* there were previous caps recorded, so compare */
    if (!gst_caps_is_equal (caps, mhclient->caps)) {
      const GValue *sh1, *sh2;

      /* caps are not equal, but could still have the same streamheader */
      s = gst_caps_get_structure (caps, 0);
      if (!gst_structure_has_field (s, "streamheader")) {
        /* no new streamheader, so nothing new to send */
        GST_DEBUG_OBJECT (sink,
            "[fd %5d] new caps do not have streamheader, not sending",
            client->fd.fd);
      } else {
        /* there is a new streamheader */
        s = gst_caps_get_structure (mhclient->caps, 0);
        if (!gst_structure_has_field (s, "streamheader")) {
          /* no previous streamheader, so send the new one */
          GST_DEBUG_OBJECT (sink,
              "[fd %5d] previous caps did not have streamheader, sending",
              client->fd.fd);
          send_streamheader = TRUE;
        } else {
          /* both old and new caps have streamheader set */
          if (!mhsink->resend_streamheader) {
            GST_DEBUG_OBJECT (sink,
                "[fd %5d] asked to not resend the streamheader, not sending",
                client->fd.fd);
            send_streamheader = FALSE;
          } else {
            sh1 = gst_structure_get_value (s, "streamheader");
            s = gst_caps_get_structure (caps, 0);
            sh2 = gst_structure_get_value (s, "streamheader");
            if (gst_value_compare (sh1, sh2) != GST_VALUE_EQUAL) {
              GST_DEBUG_OBJECT (sink,
                  "[fd %5d] new streamheader different from old, sending",
                  client->fd.fd);
              send_streamheader = TRUE;
            }
          }
        }
      }
    }
    /* Replace the old caps */
    gst_caps_unref (mhclient->caps);
    mhclient->caps = gst_caps_ref (caps);
  }

  if (G_UNLIKELY (send_streamheader)) {
    const GValue *sh;
    GArray *buffers;
    int i;

    GST_LOG_OBJECT (sink,
        "[fd %5d] sending streamheader from caps %" GST_PTR_FORMAT,
        client->fd.fd, caps);
    s = gst_caps_get_structure (caps, 0);
    if (!gst_structure_has_field (s, "streamheader")) {
      GST_DEBUG_OBJECT (sink,
          "[fd %5d] no new streamheader, so nothing to send", client->fd.fd);
    } else {
      GST_LOG_OBJECT (sink,
          "[fd %5d] sending streamheader from caps %" GST_PTR_FORMAT,
          client->fd.fd, caps);
      sh = gst_structure_get_value (s, "streamheader");
      g_assert (G_VALUE_TYPE (sh) == GST_TYPE_ARRAY);
      buffers = g_value_peek_pointer (sh);
      GST_DEBUG_OBJECT (sink, "%d streamheader buffers", buffers->len);
      for (i = 0; i < buffers->len; ++i) {
        GValue *bufval;
        GstBuffer *buffer;

        bufval = &g_array_index (buffers, GValue, i);
        g_assert (G_VALUE_TYPE (bufval) == GST_TYPE_BUFFER);
        buffer = g_value_peek_pointer (bufval);
        GST_DEBUG_OBJECT (sink,
            "[fd %5d] queueing streamheader buffer of length %" G_GSIZE_FORMAT,
            client->fd.fd, gst_buffer_get_size (buffer));
        gst_buffer_ref (buffer);

        mhclient->sending = g_slist_append (mhclient->sending, buffer);
      }
    }
  }

  gst_caps_unref (caps);
  caps = NULL;

  GST_LOG_OBJECT (sink, "[fd %5d] queueing buffer of length %" G_GSIZE_FORMAT,
      client->fd.fd, gst_buffer_get_size (buffer));

  gst_buffer_ref (buffer);
  mhclient->sending = g_slist_append (mhclient->sending, buffer);

  return TRUE;
}

/* Get the number of buffers from the buffer queue needed to satisfy
 * the maximum max in the configured units.
 * If units are not BUFFERS, and there are insufficient buffers in the
 * queue to satify the limit, return len(queue) + 1 */
static gint
get_buffers_max (GstMultiFdSink * sink, gint64 max)
{
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);

  switch (sink->unit_type) {
    case GST_TCP_UNIT_TYPE_BUFFERS:
      return max;
    case GST_TCP_UNIT_TYPE_TIME:
    {
      GstBuffer *buf;
      int i;
      int len;
      gint64 diff;
      GstClockTime first = GST_CLOCK_TIME_NONE;

      len = mhsink->bufqueue->len;

      for (i = 0; i < len; i++) {
        buf = g_array_index (mhsink->bufqueue, GstBuffer *, i);
        if (GST_BUFFER_TIMESTAMP_IS_VALID (buf)) {
          if (first == -1)
            first = GST_BUFFER_TIMESTAMP (buf);

          diff = first - GST_BUFFER_TIMESTAMP (buf);

          if (diff > max)
            return i + 1;
        }
      }
      return len + 1;
    }
    case GST_TCP_UNIT_TYPE_BYTES:
    {
      GstBuffer *buf;
      int i;
      int len;
      gint acc = 0;

      len = mhsink->bufqueue->len;

      for (i = 0; i < len; i++) {
        buf = g_array_index (mhsink->bufqueue, GstBuffer *, i);
        acc += gst_buffer_get_size (buf);

        if (acc > max)
          return i + 1;
      }
      return len + 1;
    }
    default:
      return max;
  }
}

/* find the positions in the buffer queue where *_min and *_max
 * is satisfied
 */
/* count the amount of data in the buffers and return the index
 * that satifies the given limits.
 *
 * Returns: index @idx in the buffer queue so that the given limits are
 * satisfied. TRUE if all the limits could be satisfied, FALSE if not
 * enough data was in the queue.
 *
 * FIXME, this code might now work if any of the units is in buffers...
 */
static gboolean
find_limits (GstMultiFdSink * sink,
    gint * min_idx, gint bytes_min, gint buffers_min, gint64 time_min,
    gint * max_idx, gint bytes_max, gint buffers_max, gint64 time_max)
{
  GstClockTime first, time;
  gint i, len, bytes;
  gboolean result, max_hit;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);

  /* take length of queue */
  len = mhsink->bufqueue->len;

  /* this must hold */
  g_assert (len > 0);

  GST_LOG_OBJECT (sink,
      "bytes_min %d, buffers_min %d, time_min %" GST_TIME_FORMAT
      ", bytes_max %d, buffers_max %d, time_max %" GST_TIME_FORMAT, bytes_min,
      buffers_min, GST_TIME_ARGS (time_min), bytes_max, buffers_max,
      GST_TIME_ARGS (time_max));

  /* do the trivial buffer limit test */
  if (buffers_min != -1 && len < buffers_min) {
    *min_idx = len - 1;
    *max_idx = len - 1;
    return FALSE;
  }

  result = FALSE;
  /* else count bytes and time */
  first = -1;
  bytes = 0;
  /* unset limits */
  *min_idx = -1;
  *max_idx = -1;
  max_hit = FALSE;

  i = 0;
  /* loop through the buffers, when a limit is ok, mark it 
   * as -1, we have at least one buffer in the queue. */
  do {
    GstBuffer *buf;

    /* if we checked all min limits, update result */
    if (bytes_min == -1 && time_min == -1 && *min_idx == -1) {
      /* don't go below 0 */
      *min_idx = MAX (i - 1, 0);
    }
    /* if we reached one max limit break out */
    if (max_hit) {
      /* i > 0 when we get here, we subtract one to get the position
       * of the previous buffer. */
      *max_idx = i - 1;
      /* we have valid complete result if we found a min_idx too */
      result = *min_idx != -1;
      break;
    }
    buf = g_array_index (mhsink->bufqueue, GstBuffer *, i);

    bytes += gst_buffer_get_size (buf);

    /* take timestamp and save for the base first timestamp */
    if ((time = GST_BUFFER_TIMESTAMP (buf)) != -1) {
      GST_LOG_OBJECT (sink, "Ts %" GST_TIME_FORMAT " on buffer",
          GST_TIME_ARGS (time));
      if (first == -1)
        first = time;

      /* increase max usage if we did not fill enough. Note that
       * buffers are sorted from new to old, so the first timestamp is
       * bigger than the next one. */
      if (time_min != -1 && first - time >= time_min)
        time_min = -1;
      if (time_max != -1 && first - time >= time_max)
        max_hit = TRUE;
    } else {
      GST_LOG_OBJECT (sink, "No timestamp on buffer");
    }
    /* time is OK or unknown, check and increase if not enough bytes */
    if (bytes_min != -1) {
      if (bytes >= bytes_min)
        bytes_min = -1;
    }
    if (bytes_max != -1) {
      if (bytes >= bytes_max) {
        max_hit = TRUE;
      }
    }
    i++;
  }
  while (i < len);

  /* if we did not hit the max or min limit, set to buffer size */
  if (*max_idx == -1)
    *max_idx = len - 1;
  /* make sure min does not exceed max */
  if (*min_idx == -1)
    *min_idx = *max_idx;

  return result;
}

/* parse the unit/value pair and assign it to the result value of the
 * right type, leave the other values untouched 
 *
 * Returns: FALSE if the unit is unknown or undefined. TRUE otherwise.
 */
static gboolean
assign_value (GstTCPUnitType unit, guint64 value, gint * bytes, gint * buffers,
    GstClockTime * time)
{
  gboolean res = TRUE;

  /* set only the limit of the given format to the given value */
  switch (unit) {
    case GST_TCP_UNIT_TYPE_BUFFERS:
      *buffers = (gint) value;
      break;
    case GST_TCP_UNIT_TYPE_TIME:
      *time = value;
      break;
    case GST_TCP_UNIT_TYPE_BYTES:
      *bytes = (gint) value;
      break;
    case GST_TCP_UNIT_TYPE_UNDEFINED:
    default:
      res = FALSE;
      break;
  }
  return res;
}

/* count the index in the buffer queue to satisfy the given unit
 * and value pair starting from buffer at index 0.
 *
 * Returns: TRUE if there was enough data in the queue to satisfy the
 * burst values. @idx contains the index in the buffer that contains enough
 * data to satisfy the limits or the last buffer in the queue when the
 * function returns FALSE.
 */
static gboolean
count_burst_unit (GstMultiFdSink * sink, gint * min_idx,
    GstTCPUnitType min_unit, guint64 min_value, gint * max_idx,
    GstTCPUnitType max_unit, guint64 max_value)
{
  gint bytes_min = -1, buffers_min = -1;
  gint bytes_max = -1, buffers_max = -1;
  GstClockTime time_min = GST_CLOCK_TIME_NONE, time_max = GST_CLOCK_TIME_NONE;

  assign_value (min_unit, min_value, &bytes_min, &buffers_min, &time_min);
  assign_value (max_unit, max_value, &bytes_max, &buffers_max, &time_max);

  return find_limits (sink, min_idx, bytes_min, buffers_min, time_min,
      max_idx, bytes_max, buffers_max, time_max);
}

/* decide where in the current buffer queue this new client should start
 * receiving buffers from.
 * This function is called whenever a client is connected and has not yet
 * received a buffer.
 * If this returns -1, it means that we haven't found a good point to
 * start streaming from yet, and this function should be called again later
 * when more buffers have arrived.
 */
static gint
gst_multi_fd_sink_new_client (GstMultiFdSink * sink, GstTCPClient * client)
{
  gint result;
  GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);

  GST_DEBUG_OBJECT (sink,
      "[fd %5d] new client, deciding where to start in queue", client->fd.fd);
  GST_DEBUG_OBJECT (sink, "queue is currently %d buffers long",
      mhsink->bufqueue->len);
  switch (mhclient->sync_method) {
    case GST_SYNC_METHOD_LATEST:
      /* no syncing, we are happy with whatever the client is going to get */
      result = mhclient->bufpos;
      GST_DEBUG_OBJECT (sink,
          "[fd %5d] SYNC_METHOD_LATEST, position %d", client->fd.fd, result);
      break;
    case GST_SYNC_METHOD_NEXT_KEYFRAME:
    {
      /* if one of the new buffers (between mhclient->bufpos and 0) in the queue
       * is a sync point, we can proceed, otherwise we need to keep waiting */
      GST_LOG_OBJECT (sink,
          "[fd %5d] new client, bufpos %d, waiting for keyframe", client->fd.fd,
          mhclient->bufpos);

      result = find_prev_syncframe (mhsink, mhclient->bufpos);
      if (result != -1) {
        GST_DEBUG_OBJECT (sink,
            "[fd %5d] SYNC_METHOD_NEXT_KEYFRAME: result %d",
            client->fd.fd, result);
        break;
      }

      /* client is not on a syncbuffer, need to skip these buffers and
       * wait some more */
      GST_LOG_OBJECT (sink,
          "[fd %5d] new client, skipping buffer(s), no syncpoint found",
          client->fd.fd);
      mhclient->bufpos = -1;
      break;
    }
    case GST_SYNC_METHOD_LATEST_KEYFRAME:
    {
      GST_DEBUG_OBJECT (sink,
          "[fd %5d] SYNC_METHOD_LATEST_KEYFRAME", client->fd.fd);

      /* for new clients we initially scan the complete buffer queue for
       * a sync point when a buffer is added. If we don't find a keyframe,
       * we need to wait for the next keyframe and so we change the client's
       * sync method to GST_SYNC_METHOD_NEXT_KEYFRAME.
       */
      result = find_next_syncframe (mhsink, 0);
      if (result != -1) {
        GST_DEBUG_OBJECT (sink,
            "[fd %5d] SYNC_METHOD_LATEST_KEYFRAME: result %d", client->fd.fd,
            result);
        break;
      }

      GST_DEBUG_OBJECT (sink,
          "[fd %5d] SYNC_METHOD_LATEST_KEYFRAME: no keyframe found, "
          "switching to SYNC_METHOD_NEXT_KEYFRAME", client->fd.fd);
      /* throw client to the waiting state */
      mhclient->bufpos = -1;
      /* and make client sync to next keyframe */
      mhclient->sync_method = GST_SYNC_METHOD_NEXT_KEYFRAME;
      break;
    }
    case GST_SYNC_METHOD_BURST:
    {
      gboolean ok;
      gint max;

      /* move to the position where we satisfy the client's burst
       * parameters. If we could not satisfy the parameters because there
       * is not enough data, we just send what we have (which is in result).
       * We use the max value to limit the search
       */
      ok = count_burst_unit (sink, &result, client->burst_min_unit,
          client->burst_min_value, &max, client->burst_max_unit,
          client->burst_max_value);
      GST_DEBUG_OBJECT (sink,
          "[fd %5d] SYNC_METHOD_BURST: burst_unit returned %d, result %d",
          client->fd.fd, ok, result);

      GST_LOG_OBJECT (sink, "min %d, max %d", result, max);

      /* we hit the max and it is below the min, use that then */
      if (max != -1 && max <= result) {
        result = MAX (max - 1, 0);
        GST_DEBUG_OBJECT (sink,
            "[fd %5d] SYNC_METHOD_BURST: result above max, taken down to %d",
            client->fd.fd, result);
      }
      break;
    }
    case GST_SYNC_METHOD_BURST_KEYFRAME:
    {
      gint min_idx, max_idx;
      gint next_syncframe, prev_syncframe;

      /* BURST_KEYFRAME:
       *
       * _always_ start sending a keyframe to the client. We first search
       * a keyframe between min/max limits. If there is none, we send it the
       * last keyframe before min. If there is none, the behaviour is like
       * NEXT_KEYFRAME.
       */
      /* gather burst limits */
      count_burst_unit (sink, &min_idx, client->burst_min_unit,
          client->burst_min_value, &max_idx, client->burst_max_unit,
          client->burst_max_value);

      GST_LOG_OBJECT (sink, "min %d, max %d", min_idx, max_idx);

      /* first find a keyframe after min_idx */
      next_syncframe = find_next_syncframe (mhsink, min_idx);
      if (next_syncframe != -1 && next_syncframe < max_idx) {
        /* we have a valid keyframe and it's below the max */
        GST_LOG_OBJECT (sink, "found keyframe in min/max limits");
        result = next_syncframe;
        break;
      }

      /* no valid keyframe, try to find one below min */
      prev_syncframe = find_prev_syncframe (mhsink, min_idx);
      if (prev_syncframe != -1) {
        GST_WARNING_OBJECT (sink,
            "using keyframe below min in BURST_KEYFRAME sync mode");
        result = prev_syncframe;
        break;
      }

      /* no prev keyframe or not enough data  */
      GST_WARNING_OBJECT (sink,
          "no prev keyframe found in BURST_KEYFRAME sync mode, waiting for next");

      /* throw client to the waiting state */
      mhclient->bufpos = -1;
      /* and make client sync to next keyframe */
      mhclient->sync_method = GST_SYNC_METHOD_NEXT_KEYFRAME;
      result = -1;
      break;
    }
    case GST_SYNC_METHOD_BURST_WITH_KEYFRAME:
    {
      gint min_idx, max_idx;
      gint next_syncframe;

      /* BURST_WITH_KEYFRAME:
       *
       * try to start sending a keyframe to the client. We first search
       * a keyframe between min/max limits. If there is none, we send it the
       * amount of data up 'till min.
       */
      /* gather enough data to burst */
      count_burst_unit (sink, &min_idx, client->burst_min_unit,
          client->burst_min_value, &max_idx, client->burst_max_unit,
          client->burst_max_value);

      GST_LOG_OBJECT (sink, "min %d, max %d", min_idx, max_idx);

      /* first find a keyframe after min_idx */
      next_syncframe = find_next_syncframe (mhsink, min_idx);
      if (next_syncframe != -1 && next_syncframe < max_idx) {
        /* we have a valid keyframe and it's below the max */
        GST_LOG_OBJECT (sink, "found keyframe in min/max limits");
        result = next_syncframe;
        break;
      }

      /* no keyframe, send data from min_idx */
      GST_WARNING_OBJECT (sink, "using min in BURST_WITH_KEYFRAME sync mode");

      /* make sure we don't go over the max limit */
      if (max_idx != -1 && max_idx <= min_idx) {
        result = MAX (max_idx - 1, 0);
      } else {
        result = min_idx;
      }

      break;
    }
    default:
      g_warning ("unknown sync method %d", mhclient->sync_method);
      result = mhclient->bufpos;
      break;
  }
  return result;
}

/* Handle a write on a client,
 * which indicates a read request from a client.
 *
 * For each client we maintain a queue of GstBuffers that contain the raw bytes
 * we need to send to the client.
 *
 * We first check to see if we need to send streamheaders. If so, we queue them.
 *
 * Then we run into the main loop that tries to send as many buffers as
 * possible. It will first exhaust the mhclient->sending queue and if the queue
 * is empty, it will pick a buffer from the global queue.
 *
 * Sending the buffers from the mhclient->sending queue is basically writing
 * the bytes to the socket and maintaining a count of the bytes that were
 * sent. When the buffer is completely sent, it is removed from the
 * mhclient->sending queue and we try to pick a new buffer for sending.
 *
 * When the sending returns a partial buffer we stop sending more data as
 * the next send operation could block.
 *
 * This functions returns FALSE if some error occured.
 */
static gboolean
gst_multi_fd_sink_handle_client_write (GstMultiFdSink * sink,
    GstTCPClient * client)
{
  int fd = client->fd.fd;
  gboolean more;
  gboolean flushing;
  GstClockTime now;
  GTimeVal nowtv;
  GstMultiHandleSink *mhsink;
  GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;

  mhsink = GST_MULTI_HANDLE_SINK (sink);

  g_get_current_time (&nowtv);
  now = GST_TIMEVAL_TO_TIME (nowtv);

  flushing = mhclient->status == GST_CLIENT_STATUS_FLUSHING;

  more = TRUE;
  do {
    gint maxsize;

    if (!mhclient->sending) {
      /* client is not working on a buffer */
      if (mhclient->bufpos == -1) {
        /* client is too fast, remove from write queue until new buffer is
         * available */
        gst_poll_fd_ctl_write (sink->fdset, &client->fd, FALSE);
        /* if we flushed out all of the client buffers, we can stop */
        if (mhclient->flushcount == 0)
          goto flushed;

        return TRUE;
      } else {
        /* client can pick a buffer from the global queue */
        GstBuffer *buf;
        GstClockTime timestamp;

        /* for new connections, we need to find a good spot in the
         * bufqueue to start streaming from */
        if (mhclient->new_connection && !flushing) {
          gint position = gst_multi_fd_sink_new_client (sink, client);

          if (position >= 0) {
            /* we got a valid spot in the queue */
            mhclient->new_connection = FALSE;
            mhclient->bufpos = position;
          } else {
            /* cannot send data to this client yet */
            gst_poll_fd_ctl_write (sink->fdset, &client->fd, FALSE);
            return TRUE;
          }
        }

        /* we flushed all remaining buffers, no need to get a new one */
        if (mhclient->flushcount == 0)
          goto flushed;

        /* grab buffer */
        buf = g_array_index (mhsink->bufqueue, GstBuffer *, mhclient->bufpos);
        mhclient->bufpos--;

        /* update stats */
        timestamp = GST_BUFFER_TIMESTAMP (buf);
        if (mhclient->first_buffer_ts == GST_CLOCK_TIME_NONE)
          mhclient->first_buffer_ts = timestamp;
        if (timestamp != -1)
          mhclient->last_buffer_ts = timestamp;

        /* decrease flushcount */
        if (mhclient->flushcount != -1)
          mhclient->flushcount--;

        GST_LOG_OBJECT (sink, "[fd %5d] client %p at position %d",
            fd, client, mhclient->bufpos);

        /* queueing a buffer will ref it */
        gst_multi_fd_sink_client_queue_buffer (sink, client, buf);

        /* need to start from the first byte for this new buffer */
        mhclient->bufoffset = 0;
      }
    }

    /* see if we need to send something */
    if (mhclient->sending) {
      ssize_t wrote;
      GstBuffer *head;
      GstMapInfo info;
      guint8 *data;

      /* pick first buffer from list */
      head = GST_BUFFER (mhclient->sending->data);

      g_assert (gst_buffer_map (head, &info, GST_MAP_READ));
      data = info.data;
      maxsize = info.size - mhclient->bufoffset;

      /* try to write the complete buffer */
#ifdef MSG_NOSIGNAL
#define FLAGS MSG_NOSIGNAL
#else
#define FLAGS 0
#endif
      if (client->is_socket) {
        wrote = send (fd, data + mhclient->bufoffset, maxsize, FLAGS);
      } else {
        wrote = write (fd, data + mhclient->bufoffset, maxsize);
      }
      gst_buffer_unmap (head, &info);

      if (wrote < 0) {
        /* hmm error.. */
        if (errno == EAGAIN) {
          /* nothing serious, resource was unavailable, try again later */
          more = FALSE;
        } else if (errno == ECONNRESET) {
          goto connection_reset;
        } else {
          goto write_error;
        }
      } else {
        if (wrote < maxsize) {
          /* partial write means that the client cannot read more and we should
           * stop sending more */
          GST_LOG_OBJECT (sink,
              "partial write on %d of %" G_GSSIZE_FORMAT " bytes", fd, wrote);
          mhclient->bufoffset += wrote;
          more = FALSE;
        } else {
          /* complete buffer was written, we can proceed to the next one */
          mhclient->sending = g_slist_remove (mhclient->sending, head);
          gst_buffer_unref (head);
          /* make sure we start from byte 0 for the next buffer */
          mhclient->bufoffset = 0;
        }
        /* update stats */
        mhclient->bytes_sent += wrote;
        mhclient->last_activity_time = now;
        mhsink->bytes_served += wrote;
      }
    }
  } while (more);

  return TRUE;

  /* ERRORS */
flushed:
  {
    GST_DEBUG_OBJECT (sink, "[fd %5d] flushed, removing", fd);
    mhclient->status = GST_CLIENT_STATUS_REMOVED;
    return FALSE;
  }
connection_reset:
  {
    GST_DEBUG_OBJECT (sink, "[fd %5d] connection reset by peer, removing", fd);
    mhclient->status = GST_CLIENT_STATUS_CLOSED;
    return FALSE;
  }
write_error:
  {
    GST_WARNING_OBJECT (sink,
        "[fd %5d] could not write, removing client: %s (%d)", fd,
        g_strerror (errno), errno);
    mhclient->status = GST_CLIENT_STATUS_ERROR;
    return FALSE;
  }
}

/* calculate the new position for a client after recovery. This function
 * does not update the client position but merely returns the required
 * position.
 */
static gint
gst_multi_fd_sink_recover_client (GstMultiFdSink * sink, GstTCPClient * client)
{
  gint newbufpos;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);
  GstMultiHandleClient *mhclient = (GstMultiHandleClient *) client;

  GST_WARNING_OBJECT (sink,
      "[fd %5d] client %p is lagging at %d, recover using policy %d",
      client->fd.fd, client, mhclient->bufpos, mhsink->recover_policy);

  switch (mhsink->recover_policy) {
    case GST_RECOVER_POLICY_NONE:
      /* do nothing, client will catch up or get kicked out when it reaches
       * the hard max */
      newbufpos = mhclient->bufpos;
      break;
    case GST_RECOVER_POLICY_RESYNC_LATEST:
      /* move to beginning of queue */
      newbufpos = -1;
      break;
    case GST_RECOVER_POLICY_RESYNC_SOFT_LIMIT:
      /* move to beginning of soft max */
      newbufpos = get_buffers_max (sink, sink->units_soft_max);
      break;
    case GST_RECOVER_POLICY_RESYNC_KEYFRAME:
      /* find keyframe in buffers, we search backwards to find the
       * closest keyframe relative to what this client already received. */
      newbufpos = MIN (mhsink->bufqueue->len - 1,
          get_buffers_max (sink, sink->units_soft_max) - 1);

      while (newbufpos >= 0) {
        GstBuffer *buf;

        buf = g_array_index (mhsink->bufqueue, GstBuffer *, newbufpos);
        if (is_sync_frame (mhsink, buf)) {
          /* found a buffer that is not a delta unit */
          break;
        }
        newbufpos--;
      }
      break;
    default:
      /* unknown recovery procedure */
      newbufpos = get_buffers_max (sink, sink->units_soft_max);
      break;
  }
  return newbufpos;
}

/* Queue a buffer on the global queue.
 *
 * This function adds the buffer to the front of a GArray. It removes the
 * tail buffer if the max queue size is exceeded, unreffing the queued buffer.
 * Note that unreffing the buffer is not a problem as clients who
 * started writing out this buffer will still have a reference to it in the
 * mhclient->sending queue.
 *
 * After adding the buffer, we update all client positions in the queue. If
 * a client moves over the soft max, we start the recovery procedure for this
 * slow client. If it goes over the hard max, it is put into the slow list
 * and removed.
 *
 * Special care is taken of clients that were waiting for a new buffer (they
 * had a position of -1) because they can proceed after adding this new buffer.
 * This is done by adding the client back into the write fd_set and signaling
 * the select thread that the fd_set changed.
 */
static void
gst_multi_fd_sink_queue_buffer (GstMultiFdSink * sink, GstBuffer * buf)
{
  GList *clients, *next;
  gint queuelen;
  gboolean need_signal = FALSE;
  gint max_buffer_usage;
  gint i;
  GTimeVal nowtv;
  GstClockTime now;
  gint max_buffers, soft_max_buffers;
  guint cookie;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);
  GstMultiHandleSinkClass *mhsinkclass =
      GST_MULTI_HANDLE_SINK_GET_CLASS (mhsink);

  g_get_current_time (&nowtv);
  now = GST_TIMEVAL_TO_TIME (nowtv);

  CLIENTS_LOCK (sink);
  /* add buffer to queue */
  g_array_prepend_val (mhsink->bufqueue, buf);
  queuelen = mhsink->bufqueue->len;

  if (sink->units_max > 0)
    max_buffers = get_buffers_max (sink, sink->units_max);
  else
    max_buffers = -1;

  if (sink->units_soft_max > 0)
    soft_max_buffers = get_buffers_max (sink, sink->units_soft_max);
  else
    soft_max_buffers = -1;
  GST_LOG_OBJECT (sink, "Using max %d, softmax %d", max_buffers,
      soft_max_buffers);

  /* then loop over the clients and update the positions */
  max_buffer_usage = 0;

restart:
  cookie = mhsink->clients_cookie;
  for (clients = mhsink->clients; clients; clients = next) {
    GstTCPClient *client;
    GstMultiHandleClient *mhclient;

    if (cookie != mhsink->clients_cookie) {
      GST_DEBUG_OBJECT (sink, "Clients cookie outdated, restarting");
      goto restart;
    }

    client = (GstTCPClient *) clients->data;
    mhclient = (GstMultiHandleClient *) client;
    next = g_list_next (clients);

    mhclient->bufpos++;
    GST_LOG_OBJECT (sink, "[fd %5d] client %p at position %d",
        client->fd.fd, client, mhclient->bufpos);
    /* check soft max if needed, recover client */
    if (soft_max_buffers > 0 && mhclient->bufpos >= soft_max_buffers) {
      gint newpos;

      newpos = gst_multi_fd_sink_recover_client (sink, client);
      if (newpos != mhclient->bufpos) {
        mhclient->dropped_buffers += mhclient->bufpos - newpos;
        mhclient->bufpos = newpos;
        mhclient->discont = TRUE;
        GST_INFO_OBJECT (sink, "[fd %5d] client %p position reset to %d",
            client->fd.fd, client, mhclient->bufpos);
      } else {
        GST_INFO_OBJECT (sink,
            "[fd %5d] client %p not recovering position",
            client->fd.fd, client);
      }
    }
    /* check hard max and timeout, remove client */
    if ((max_buffers > 0 && mhclient->bufpos >= max_buffers) ||
        (mhsink->timeout > 0
            && now - mhclient->last_activity_time > mhsink->timeout)) {
      /* remove client */
      GST_WARNING_OBJECT (sink, "[fd %5d] client %p is too slow, removing",
          client->fd.fd, client);
      /* remove the client, the fd set will be cleared and the select thread
       * will be signaled */
      mhclient->status = GST_CLIENT_STATUS_SLOW;
      /* set client to invalid position while being removed */
      mhclient->bufpos = -1;
      mhsinkclass->remove_client_link (mhsink, clients);
      need_signal = TRUE;
      continue;
    } else if (mhclient->bufpos == 0 || mhclient->new_connection) {
      /* can send data to this client now. need to signal the select thread that
       * the fd_set changed */
      gst_poll_fd_ctl_write (sink->fdset, &client->fd, TRUE);
      need_signal = TRUE;
    }
    /* keep track of maximum buffer usage */
    if (mhclient->bufpos > max_buffer_usage) {
      max_buffer_usage = mhclient->bufpos;
    }
  }

  /* make sure we respect bytes-min, buffers-min and time-min when they are set */
  {
    gint usage, max;

    GST_LOG_OBJECT (sink,
        "extending queue %d to respect time_min %" GST_TIME_FORMAT
        ", bytes_min %d, buffers_min %d", max_buffer_usage,
        GST_TIME_ARGS (mhsink->time_min), mhsink->bytes_min,
        mhsink->buffers_min);

    /* get index where the limits are ok, we don't really care if all limits
     * are ok, we just queue as much as we need. We also don't compare against
     * the max limits. */
    find_limits (sink, &usage, mhsink->bytes_min, mhsink->buffers_min,
        mhsink->time_min, &max, -1, -1, -1);

    max_buffer_usage = MAX (max_buffer_usage, usage + 1);
    GST_LOG_OBJECT (sink, "extended queue to %d", max_buffer_usage);
  }

  /* now look for sync points and make sure there is at least one
   * sync point in the queue. We only do this if the LATEST_KEYFRAME or 
   * BURST_KEYFRAME mode is selected */
  if (mhsink->def_sync_method == GST_SYNC_METHOD_LATEST_KEYFRAME ||
      mhsink->def_sync_method == GST_SYNC_METHOD_BURST_KEYFRAME) {
    /* no point in searching beyond the queue length */
    gint limit = queuelen;
    GstBuffer *buf;

    /* no point in searching beyond the soft-max if any. */
    if (soft_max_buffers > 0) {
      limit = MIN (limit, soft_max_buffers);
    }
    GST_LOG_OBJECT (sink,
        "extending queue to include sync point, now at %d, limit is %d",
        max_buffer_usage, limit);
    for (i = 0; i < limit; i++) {
      buf = g_array_index (mhsink->bufqueue, GstBuffer *, i);
      if (is_sync_frame (mhsink, buf)) {
        /* found a sync frame, now extend the buffer usage to
         * include at least this frame. */
        max_buffer_usage = MAX (max_buffer_usage, i);
        break;
      }
    }
    GST_LOG_OBJECT (sink, "max buffer usage is now %d", max_buffer_usage);
  }

  GST_LOG_OBJECT (sink, "len %d, usage %d", queuelen, max_buffer_usage);

  /* nobody is referencing units after max_buffer_usage so we can
   * remove them from the queue. We remove them in reverse order as
   * this is the most optimal for GArray. */
  for (i = queuelen - 1; i > max_buffer_usage; i--) {
    GstBuffer *old;

    /* queue exceeded max size */
    queuelen--;
    old = g_array_index (mhsink->bufqueue, GstBuffer *, i);
    mhsink->bufqueue = g_array_remove_index (mhsink->bufqueue, i);

    /* unref tail buffer */
    gst_buffer_unref (old);
  }
  /* save for stats */
  mhsink->buffers_queued = max_buffer_usage;
  CLIENTS_UNLOCK (sink);

  /* and send a signal to thread if fd_set changed */
  if (need_signal) {
    gst_poll_restart (sink->fdset);
  }
}

/* Handle the clients. Basically does a blocking select for one
 * of the client fds to become read or writable. We also have a
 * filedescriptor to receive commands on that we need to check.
 *
 * After going out of the select call, we read and write to all
 * clients that can do so. Badly behaving clients are put on a
 * garbage list and removed.
 */
static void
gst_multi_fd_sink_handle_clients (GstMultiFdSink * sink)
{
  int result;
  GList *clients, *next;
  gboolean try_again;
  GstMultiFdSinkClass *fclass;
  guint cookie;
  GstMultiHandleSink *mhsink = GST_MULTI_HANDLE_SINK (sink);
  GstMultiHandleSinkClass *mhsinkclass =
      GST_MULTI_HANDLE_SINK_GET_CLASS (mhsink);


  fclass = GST_MULTI_FD_SINK_GET_CLASS (sink);

  do {
    try_again = FALSE;

    /* check for:
     * - server socket input (ie, new client connections)
     * - client socket input (ie, clients saying goodbye)
     * - client socket output (ie, client reads)          */
    GST_LOG_OBJECT (sink, "waiting on action on fdset");

    result =
        gst_poll_wait (sink->fdset,
        mhsink->timeout != 0 ? mhsink->timeout : GST_CLOCK_TIME_NONE);

    /* Handle the special case in which the sink is not receiving more buffers
     * and will not disconnect inactive client in the streaming thread. */
    if (G_UNLIKELY (result == 0)) {
      GstClockTime now;
      GTimeVal nowtv;

      g_get_current_time (&nowtv);
      now = GST_TIMEVAL_TO_TIME (nowtv);

      CLIENTS_LOCK (sink);
      for (clients = mhsink->clients; clients; clients = next) {
        GstTCPClient *client;
        GstMultiHandleClient *mhclient;

        client = (GstTCPClient *) clients->data;
        mhclient = (GstMultiHandleClient *) client;
        next = g_list_next (clients);
        if (mhsink->timeout > 0
            && now - mhclient->last_activity_time > mhsink->timeout) {
          mhclient->status = GST_CLIENT_STATUS_SLOW;
          mhsinkclass->remove_client_link (mhsink, clients);
        }
      }
      CLIENTS_UNLOCK (sink);
      return;
    } else if (result < 0) {
      GST_WARNING_OBJECT (sink, "wait failed: %s (%d)", g_strerror (errno),
          errno);
      if (errno == EBADF) {
        /* ok, so one or more of the fds is invalid. We loop over them to find
         * the ones that give an error to the F_GETFL fcntl. */
        CLIENTS_LOCK (sink);
      restart:
        cookie = mhsink->clients_cookie;
        for (clients = mhsink->clients; clients; clients = next) {
          GstTCPClient *client;
          GstMultiHandleClient *mhclient;
          int fd;
          long flags;
          int res;

          if (cookie != mhsink->clients_cookie) {
            GST_DEBUG_OBJECT (sink, "Cookie changed finding bad fd");
            goto restart;
          }

          client = (GstTCPClient *) clients->data;
          mhclient = (GstMultiHandleClient *) client;
          next = g_list_next (clients);

          fd = client->fd.fd;

          res = fcntl (fd, F_GETFL, &flags);
          if (res == -1) {
            GST_WARNING_OBJECT (sink, "fnctl failed for %d, removing: %s (%d)",
                fd, g_strerror (errno), errno);
            if (errno == EBADF) {
              mhclient->status = GST_CLIENT_STATUS_ERROR;
              /* releases the CLIENTS lock */
              mhsinkclass->remove_client_link (mhsink, clients);
            }
          }
        }
        CLIENTS_UNLOCK (sink);
        /* after this, go back in the select loop as the read/writefds
         * are not valid */
        try_again = TRUE;
      } else if (errno == EINTR) {
        /* interrupted system call, just redo the wait */
        try_again = TRUE;
      } else if (errno == EBUSY) {
        /* the call to gst_poll_wait() was flushed */
        return;
      } else {
        /* this is quite bad... */
        GST_ELEMENT_ERROR (sink, RESOURCE, READ, (NULL),
            ("select failed: %s (%d)", g_strerror (errno), errno));
        return;
      }
    } else {
      GST_LOG_OBJECT (sink, "wait done: %d sockets with events", result);
    }
  } while (try_again);

  /* subclasses can check fdset with this virtual function */
  if (fclass->wait)
    fclass->wait (sink, sink->fdset);

  /* Check the clients */
  CLIENTS_LOCK (sink);

restart2:
  cookie = mhsink->clients_cookie;
  for (clients = mhsink->clients; clients; clients = next) {
    GstTCPClient *client;
    GstMultiHandleClient *mhclient;

    if (mhsink->clients_cookie != cookie) {
      GST_DEBUG_OBJECT (sink, "Restarting loop, cookie out of date");
      goto restart2;
    }

    client = (GstTCPClient *) clients->data;
    mhclient = (GstMultiHandleClient *) client;
    next = g_list_next (clients);

    if (mhclient->status != GST_CLIENT_STATUS_FLUSHING
        && mhclient->status != GST_CLIENT_STATUS_OK) {
      mhsinkclass->remove_client_link (mhsink, clients);
      continue;
    }

    if (gst_poll_fd_has_closed (sink->fdset, &client->fd)) {
      mhclient->status = GST_CLIENT_STATUS_CLOSED;
      mhsinkclass->remove_client_link (mhsink, clients);
      continue;
    }
    if (gst_poll_fd_has_error (sink->fdset, &client->fd)) {
      GST_WARNING_OBJECT (sink, "gst_poll_fd_has_error for %d", client->fd.fd);
      mhclient->status = GST_CLIENT_STATUS_ERROR;
      mhsinkclass->remove_client_link (mhsink, clients);
      continue;
    }
    if (gst_poll_fd_can_read (sink->fdset, &client->fd)) {
      /* handle client read */
      if (!gst_multi_fd_sink_handle_client_read (sink, client)) {
        mhsinkclass->remove_client_link (mhsink, clients);
        continue;
      }
    }
    if (gst_poll_fd_can_write (sink->fdset, &client->fd)) {
      /* handle client write */
      if (!gst_multi_fd_sink_handle_client_write (sink, client)) {
        mhsinkclass->remove_client_link (mhsink, clients);
        continue;
      }
    }
  }
  CLIENTS_UNLOCK (sink);
}

/* we handle the client communication in another thread so that we do not block
 * the gstreamer thread while we select() on the client fds */
static gpointer
gst_multi_fd_sink_thread (GstMultiHandleSink * mhsink)
{
  GstMultiFdSink *sink = GST_MULTI_FD_SINK (mhsink);

  while (mhsink->running) {
    gst_multi_fd_sink_handle_clients (sink);
  }
  return NULL;
}

static GstFlowReturn
gst_multi_fd_sink_render (GstBaseSink * bsink, GstBuffer * buf)
{
  GstMultiFdSink *sink;
  gboolean in_caps;
#if 0
  GstCaps *bufcaps, *padcaps;
#endif
  GstMultiHandleSink *mhsink;

  sink = GST_MULTI_FD_SINK (bsink);
  mhsink = GST_MULTI_HANDLE_SINK (sink);

  g_return_val_if_fail (GST_OBJECT_FLAG_IS_SET (sink,
          GST_MULTI_HANDLE_SINK_OPEN), GST_FLOW_FLUSHING);

#if 0
  /* since we check every buffer for streamheader caps, we need to make
   * sure every buffer has caps set */
  bufcaps = gst_buffer_get_caps (buf);
  padcaps = GST_PAD_CAPS (GST_BASE_SINK_PAD (bsink));

  /* make sure we have caps on the pad */
  if (!padcaps && !bufcaps)
    goto no_caps;
#endif

  /* get HEADER first, code below might mess with the flags */
  in_caps = GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_HEADER);

#if 0
  /* stamp the buffer with previous caps if no caps set */
  if (!bufcaps) {
    if (!gst_buffer_is_writable (buf)) {
      /* metadata is not writable, copy will be made and original buffer
       * will be unreffed so we need to ref so that we don't lose the
       * buffer in the render method. */
      gst_buffer_ref (buf);
      /* the new buffer is ours only, we keep it out of the scope of this
       * function */
      buf = gst_buffer_make_writable (buf);
    } else {
      /* else the metadata is writable, we ref because we keep the buffer
       * out of the scope of this method */
      gst_buffer_ref (buf);
    }
    /* buffer metadata is writable now, set the caps */
    gst_buffer_set_caps (buf, padcaps);
  } else {
    gst_caps_unref (bufcaps);

    /* since we keep this buffer out of the scope of this method */
    gst_buffer_ref (buf);
  }
#endif
  gst_buffer_ref (buf);

  GST_LOG_OBJECT (sink, "received buffer %p, in_caps: %s, offset %"
      G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT
      ", timestamp %" GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT,
      buf, in_caps ? "yes" : "no", GST_BUFFER_OFFSET (buf),
      GST_BUFFER_OFFSET_END (buf),
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (buf)));

  /* if we get HEADER buffers, but the previous buffer was not HEADER,
   * it means we're getting new streamheader buffers, and we should clear
   * the old ones */
  if (in_caps && sink->previous_buffer_in_caps == FALSE) {
    GST_DEBUG_OBJECT (sink,
        "receiving new HEADER buffers, clearing old streamheader");
    g_slist_foreach (mhsink->streamheader, (GFunc) gst_mini_object_unref, NULL);
    g_slist_free (mhsink->streamheader);
    mhsink->streamheader = NULL;
  }

  /* save the current in_caps */
  sink->previous_buffer_in_caps = in_caps;

  /* if the incoming buffer is marked as IN CAPS, then we assume for now
   * it's a streamheader that needs to be sent to each new client, so we
   * put it on our internal list of streamheader buffers.
   * FIXME: we could check if the buffer's contents are in fact part of the
   * current streamheader.
   *
   * We don't send the buffer to the client, since streamheaders are sent
   * separately when necessary. */
  if (in_caps) {
    GST_DEBUG_OBJECT (sink, "appending HEADER buffer with length %"
        G_GSIZE_FORMAT " to streamheader", gst_buffer_get_size (buf));
    mhsink->streamheader = g_slist_append (mhsink->streamheader, buf);
  } else {
    /* queue the buffer, this is a regular data buffer. */
    gst_multi_fd_sink_queue_buffer (sink, buf);

    mhsink->bytes_to_serve += gst_buffer_get_size (buf);
  }
  return GST_FLOW_OK;

  /* ERRORS */
#if 0
no_caps:
  {
    GST_ELEMENT_ERROR (sink, CORE, NEGOTIATION, (NULL),
        ("Received first buffer without caps set"));
    return GST_FLOW_NOT_NEGOTIATED;
  }
#endif
}

static void
gst_multi_fd_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMultiFdSink *multifdsink;

  multifdsink = GST_MULTI_FD_SINK (object);

  switch (prop_id) {
    case PROP_MODE:
      multifdsink->mode = g_value_get_enum (value);
      break;
    case PROP_BUFFERS_MAX:
      multifdsink->units_max = g_value_get_int (value);
      break;
    case PROP_BUFFERS_SOFT_MAX:
      multifdsink->units_soft_max = g_value_get_int (value);
      break;
    case PROP_UNIT_TYPE:
      multifdsink->unit_type = g_value_get_enum (value);
      break;
    case PROP_UNITS_MAX:
      multifdsink->units_max = g_value_get_int64 (value);
      break;
    case PROP_UNITS_SOFT_MAX:
      multifdsink->units_soft_max = g_value_get_int64 (value);
      break;
    case PROP_BURST_UNIT:
      multifdsink->def_burst_unit = g_value_get_enum (value);
      break;
    case PROP_BURST_VALUE:
      multifdsink->def_burst_value = g_value_get_uint64 (value);
      break;
    case PROP_QOS_DSCP:
      multifdsink->qos_dscp = g_value_get_int (value);
      setup_dscp (multifdsink);
      break;
    case PROP_HANDLE_READ:
      multifdsink->handle_read = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_multi_fd_sink_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMultiFdSink *multifdsink;

  multifdsink = GST_MULTI_FD_SINK (object);

  switch (prop_id) {
    case PROP_MODE:
      g_value_set_enum (value, multifdsink->mode);
      break;
    case PROP_BUFFERS_MAX:
      g_value_set_int (value, multifdsink->units_max);
      break;
    case PROP_BUFFERS_SOFT_MAX:
      g_value_set_int (value, multifdsink->units_soft_max);
      break;
    case PROP_UNIT_TYPE:
      g_value_set_enum (value, multifdsink->unit_type);
      break;
    case PROP_UNITS_MAX:
      g_value_set_int64 (value, multifdsink->units_max);
      break;
    case PROP_UNITS_SOFT_MAX:
      g_value_set_int64 (value, multifdsink->units_soft_max);
      break;
    case PROP_BURST_UNIT:
      g_value_set_enum (value, multifdsink->def_burst_unit);
      break;
    case PROP_BURST_VALUE:
      g_value_set_uint64 (value, multifdsink->def_burst_value);
      break;
    case PROP_QOS_DSCP:
      g_value_set_int (value, multifdsink->qos_dscp);
      break;
    case PROP_HANDLE_READ:
      g_value_set_boolean (value, multifdsink->handle_read);
      break;
    case PROP_NUM_FDS:
      g_value_set_uint (value, g_hash_table_size (multifdsink->fd_hash));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_multi_fd_sink_start_pre (GstMultiHandleSink * mhsink)
{
  GstMultiFdSink *mfsink = GST_MULTI_FD_SINK (mhsink);

  GST_INFO_OBJECT (mfsink, "starting in mode %d", mfsink->mode);
  if ((mfsink->fdset = gst_poll_new (TRUE)) == NULL)
    goto socket_pair;

  return TRUE;

  /* ERRORS */
socket_pair:
  {
    GST_ELEMENT_ERROR (mfsink, RESOURCE, OPEN_READ_WRITE, (NULL),
        GST_ERROR_SYSTEM);
    return FALSE;
  }
}

static gboolean
multifdsink_hash_remove (gpointer key, gpointer value, gpointer data)
{
  return TRUE;
}

static void
gst_multi_fd_sink_stop_pre (GstMultiHandleSink * mhsink)
{
  GstMultiFdSink *mfsink = GST_MULTI_FD_SINK (mhsink);

  gst_poll_set_flushing (mfsink->fdset, TRUE);

}

static void
gst_multi_fd_sink_stop_post (GstMultiHandleSink * mhsink)
{
  GstMultiFdSink *mfsink = GST_MULTI_FD_SINK (mhsink);

  if (mfsink->fdset) {
    gst_poll_free (mfsink->fdset);
    mfsink->fdset = NULL;
  }
  g_hash_table_foreach_remove (mfsink->fd_hash, multifdsink_hash_remove,
      mfsink);
}
