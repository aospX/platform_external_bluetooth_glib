#include <unistd.h>
#include <glib.h>
#include <glib-object.h>

#define G_TYPE_TEST               (g_test_get_type ())
#define G_TEST(test)              (G_TYPE_CHECK_INSTANCE_CAST ((test), G_TYPE_TEST, GTest))
#define G_IS_TEST(test)           (G_TYPE_CHECK_INSTANCE_TYPE ((test), G_TYPE_TEST))
#define G_TEST_CLASS(tclass)      (G_TYPE_CHECK_CLASS_CAST ((tclass), G_TYPE_TEST, GTestClass))
#define G_IS_TEST_CLASS(tclass)   (G_TYPE_CHECK_CLASS_TYPE ((tclass), G_TYPE_TEST))
#define G_TEST_GET_CLASS(test)    (G_TYPE_INSTANCE_GET_CLASS ((test), G_TYPE_TEST, GTestClass))

enum {
  PROP_0,
  PROP_DUMMY
};

typedef struct _GTest GTest;
typedef struct _GTestClass GTestClass;

struct _GTest
{
  GObject object;

  gint dummy;

  gint count;
};

struct _GTestClass
{
  GObjectClass parent_class;
};

static GType g_test_get_type (void);
static gboolean stopping;

static void g_test_class_init (GTestClass * klass);
static void g_test_init (GTest * test);
static void g_test_dispose (GObject * object);
static void g_test_get_property (GObject    *object,
				 guint       prop_id,
				 GValue     *value,
				 GParamSpec *pspec);
static void g_test_set_property (GObject      *object,
				 guint         prop_id,
				 const GValue *value,
				 GParamSpec   *pspec);

static GObjectClass *parent_class = NULL;

static GType
g_test_get_type (void)
{
  static GType test_type = 0;

  if (!test_type) {
    static const GTypeInfo test_info = {
      sizeof (GTestClass),
      NULL,
      NULL,
      (GClassInitFunc) g_test_class_init,
      NULL,
      NULL,
      sizeof (GTest),
      0,
      (GInstanceInitFunc) g_test_init,
      NULL
    };

    test_type = g_type_register_static (G_TYPE_OBJECT, "GTest",
        &test_info, 0);
  }
  return test_type;
}

static void
g_test_class_init (GTestClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  parent_class = g_type_class_ref (G_TYPE_OBJECT);

  gobject_class->dispose = g_test_dispose;
  gobject_class->get_property = g_test_get_property;
  gobject_class->set_property = g_test_set_property;

  g_object_class_install_property (gobject_class,
				   PROP_DUMMY,
				   g_param_spec_int ("dummy",
						     NULL, 
						     NULL,
						     0, G_MAXINT, 0,
						     G_PARAM_READWRITE));
}

static void
g_test_init (GTest * test)
{
  g_print ("init %p\n", test);
}

static void
g_test_dispose (GObject * object)
{
  GTest *test;

  test = G_TEST (object);

  g_print ("dispose %p!\n", object);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void 
g_test_get_property (GObject    *object,
		     guint       prop_id,
		     GValue     *value,
		     GParamSpec *pspec)
{
  GTest *test;

  test = G_TEST (object);

  switch (prop_id)
    {
    case PROP_DUMMY:
      g_value_set_int (value, test->dummy);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void 
g_test_set_property (GObject      *object,
		     guint         prop_id,
		     const GValue *value,
		     GParamSpec   *pspec)
{
  GTest *test;

  test = G_TEST (object);

  switch (prop_id)
    {
    case PROP_DUMMY:
      test->dummy = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
dummy_notify (GObject    *object,
              GParamSpec *pspec)
{
  GTest *test;

  test = G_TEST (object);

  test->count++;  
}

static void
g_test_do_property (GTest * test)
{
  gint dummy;

  g_object_get (test, "dummy", &dummy, NULL);
  g_object_set (test, "dummy", dummy + 1, NULL);
}

static gpointer
run_thread (GTest * test)
{
  gint i = 0;

  while (!stopping) {
    g_test_do_property (test);
    if ((i++ % 100000) == 0) {
      g_print (".");
      g_usleep (1);             /* context switch */
    }
  }

  return NULL;
}

int
main (int argc, char **argv)
{
  gint i;
  GArray *test_objects;
  GArray *test_threads;

  g_thread_init (NULL);
  g_type_init ();

  test_objects = g_array_new (FALSE, FALSE, sizeof (GTest *));

  for (i = 0; i < 20; i++) {
    GTest *test;
    
    test = g_object_new (G_TYPE_TEST, NULL);
    g_array_append_val (test_objects, test);

    g_assert (test->count == test->dummy);
    g_signal_connect (test, "notify::dummy", G_CALLBACK (dummy_notify), NULL);
  }
    
  test_threads = g_array_new (FALSE, FALSE, sizeof (GThread *));

  stopping = FALSE;

  for (i = 0; i < 20; i++) {
    GThread *thread;
    GTest *test;

    test = g_array_index (test_objects, GTest *, i);

    thread = g_thread_create ((GThreadFunc) run_thread, test, TRUE, NULL);
    g_array_append_val (test_threads, thread);
  }
  sleep (5);

  stopping = TRUE;

  g_print ("\nstopping\n");

  /* join all threads */
  for (i = 0; i < 20; i++) {
    GThread *thread;

    thread = g_array_index (test_threads, GThread *, i);
    g_thread_join (thread);
  }

  g_print ("stopped\n");

  for (i = 0; i < 20; i++) {
    GTest *test;

    test = g_array_index (test_objects, GTest *, i);

    g_assert (test->count == test->dummy);
  }

  return 0;
}