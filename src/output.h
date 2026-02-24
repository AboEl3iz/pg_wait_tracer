/* output.h — CLI output formatters for 4 diagnostic views */
#ifndef PGWT_OUTPUT_H
#define PGWT_OUTPUT_H

struct pgwt_daemon;

void pgwt_print_header(struct pgwt_daemon *d);
void pgwt_print_time_model(struct pgwt_daemon *d);
void pgwt_print_system_event(struct pgwt_daemon *d);
void pgwt_print_session_event(struct pgwt_daemon *d);
void pgwt_print_histogram(struct pgwt_daemon *d);
void pgwt_print_query_event(struct pgwt_daemon *d);
void pgwt_print_active(struct pgwt_daemon *d);

#endif /* PGWT_OUTPUT_H */
