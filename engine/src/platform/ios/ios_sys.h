// iOS platform helpers that need system headers (<unistd.h>, <sys/stat.h>)
// which clash with generated names, so ios_sys.c does not include ik_sim.h.
#ifndef CVF_IOS_SYS_H
#define CVF_IOS_SYS_H

int cvf_ios_on_main_thread(void);
// Log file (<assets>/save/logs/cvf.log), save/cvf_env.txt, stack report.
void cvf_ios_prepare(const char *assets);
int cvf_ios_chdir(const char *dir);
void cvf_ios_frame_tick(void);  // once per frame (stack high-water mark)
// Background/foreground (IOS_PORT.md 3.7): no GL work while backgrounded.
void cvf_ios_install_app_watch(void);
void cvf_ios_wait_foreground(void);  // returns at once when in the foreground

// cvf_host.c
void cvf_host_set_running(int up);
void cvf_host_drain(void);

#endif
