/**
 * 监控运行情况
 */
#include <glib/glib.h>

/*监控执行情况*/
typedef struct s_dev189_timer
{
    char *name;
    gint64 start;
    gint64 end;
    gint64 elapse;
    uint16_t counter; // 调用的次数
    /*below are private fields*/
    gint64 last_start;
} t_dev189_timer;

typedef struct s_dev189_monitor
{
    GHashTable *timers;
} t_dev189_monitor;

t_dev189_monitor *dev189_monitor_new();

t_dev189_timer *dev189_monitor_timer_new(t_dev189_monitor *mon, const char *timer_name);

void dev189_monitor_timer_free(gpointer trace);

void dev189_monitor_free(t_dev189_monitor *mon);

void dev189_monitor_timer_on(t_dev189_monitor *mon, const char *timer_name);

void dev189_monitor_timer_off(t_dev189_monitor *mon, const char *timer_name);

gchar *dev189_monitor_timer_str(t_dev189_monitor *mon, const char *timer_name);