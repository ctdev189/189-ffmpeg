/**
 * 接收RTP 
 */
#include "monitor.h"

t_dev189_monitor *dev189_monitor_new()
{
    t_dev189_monitor *mon = g_malloc0(sizeof(t_dev189_monitor));
    mon->timers = g_hash_table_new(g_str_hash, g_str_equal);
    return mon;
}

t_dev189_timer *dev189_monitor_timer_new(t_dev189_monitor *mon, const char *timer_name)
{
    t_dev189_timer *timer = g_malloc0(sizeof(t_dev189_timer));
    timer->name = g_strdup(timer_name);
    g_hash_table_insert(mon->timers, (gpointer *)timer_name, timer);

    return timer;
}

void dev189_monitor_timer_free(gpointer timer)
{
    g_free(timer);
    timer = NULL;
}

void dev189_monitor_free(t_dev189_monitor *mon)
{
    g_free(mon);
}

void dev189_monitor_timer_on(t_dev189_monitor *mon, const char *timer_name)
{
    t_dev189_timer *timer = (t_dev189_timer *)g_hash_table_lookup(mon->timers, timer_name);
    if (timer)
    {
        timer->last_start = g_get_monotonic_time();
        if (!timer->start)
            timer->start = timer->last_start;
        timer->counter++;
    }
}

void dev189_monitor_timer_off(t_dev189_monitor *mon, const char *timer_name)
{
    t_dev189_timer *timer = (t_dev189_timer *)g_hash_table_lookup(mon->timers, timer_name);
    if (timer)
    {
        timer->end = g_get_monotonic_time();
        timer->elapse += timer->end - timer->last_start;
    }
}

gchar *dev189_monitor_timer_str(t_dev189_monitor *mon, const char *timer_name)
{
    GString *s = g_string_new("");
    t_dev189_timer *timer = (t_dev189_timer *)g_hash_table_lookup(mon->timers, timer_name);
    if (timer)
    {
        g_string_printf(s, "timer=%s elapse=%lld(ms) / %lld(s) times=%u",
                        timer->name, timer->elapse, timer->elapse / G_USEC_PER_SEC, timer->counter);
    }
    return s->str;
}