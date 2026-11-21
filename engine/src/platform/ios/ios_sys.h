// iOS platform helpers that need system headers (<unistd.h>, <sys/stat.h>)
// which clash with generated names, so ios_sys.c does not include ik_sim.h.
#ifndef CVF_IOS_SYS_H
#define CVF_IOS_SYS_H
#include <stdbool.h>

int cvf_ios_on_main_thread(void);
// Log file (<assets>/save/logs/cvf.log), save/cvf_env.txt, stack report.
void cvf_ios_prepare(const char *assets);
int cvf_ios_chdir(const char *dir);
// Runs fn(arg) on the engine's own 8 MB stack (plan B of IOS_PORT.md 3.3).
int cvf_ios_run_on_engine_stack(void (*fn)(void *), void *arg);
void *cvf_ios_engine_stack_top(void);
void cvf_ios_frame_tick(void);  // once per frame (stack high-water mark)
// Background/foreground (IOS_PORT.md 3.7): no GL work while backgrounded.
void cvf_ios_install_app_watch(void);
void cvf_ios_wait_foreground(void);  // returns at once when in the foreground
int cvf_ios_take_low_memory(void);    // 1 once after each memory warning
// ios_view.m: SDL's GL view into the app's window, after GL context creation
void cvf_ios_attach_gl_view(void *sdl_window);

// cvf_host.c
void cvf_host_set_running(int up);
void cvf_host_drain(bool fighting);

#endif
