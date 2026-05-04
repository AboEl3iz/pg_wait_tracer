Output demos/dump.gif
Set FontSize 14
Set Width 1200
Set Height 780
Set Theme "Dracula"
Set Shell "zsh"
Set TypingSpeed 25ms

Hide
Type "cd /tmp/pgwt-demo && setopt interactive_comments && export PATH=/Users/nik/github/pg_wait_tracer:$PATH"
Enter
Type "clear"
Enter
Show

Type "# pg_wait_tracer — offline replay of a 60s pgbench trace"
Enter
Sleep 1500ms
Type "# 7M wait-event transitions, captured via eBPF hardware watchpoint"
Enter
Sleep 1500ms
Type "ls traces/"
Enter
Sleep 2500ms

Type "pgwt-server --dump traces 2>/dev/null | head -60"
Enter
Sleep 10s

Type "clear"
Enter
Sleep 300ms

Type "# Top sessions + top queries (by query_id)"
Enter
Sleep 1200ms
Type "pgwt-server --dump traces 2>/dev/null | tail -40"
Enter
Sleep 10s
