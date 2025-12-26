#include <dirent.h>
#include <fcntl.h>
#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* * Optimization Macros: 
 * These hints help the compiler optimize branch prediction, improving CPU pipeline efficiency. 
 */
#define TERMUX_UNUSED(x) x __attribute__((__unused__))
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#ifdef __APPLE__
# define LACKS_PTSNAME_R
#endif

/* * Helper function to throw a Java runtime exception.
 * Marked as 'cold' to hint the compiler that this path is rarely taken, 
 * optimizing the instruction cache for the main execution path.
 */
static int __attribute__((cold)) throw_runtime_exception(JNIEnv* env, char const* message)
{
    jclass exClass = (*env)->FindClass(env, "java/lang/RuntimeException");
    (*env)->ThrowNew(env, exClass, message);
    return -1;
}

static int create_subprocess(JNIEnv* env,
        char const* cmd,
        char const* cwd,
        char* const argv[],
        char** envp,
        int* pProcessId,
        jint rows,
        jint columns,
        jint cell_width,
        jint cell_height)
{
    // Open the pseudo-terminal master.
    int ptm = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
    if (unlikely(ptm < 0)) return throw_runtime_exception(env, "Cannot open /dev/ptmx");

#ifdef LACKS_PTSNAME_R
    char* devname;
#else
    char devname[64];
#endif
    // Grant access to the slave pseudo-terminal and get its name.
    if (unlikely(grantpt(ptm) || unlockpt(ptm) ||
#ifdef LACKS_PTSNAME_R
            (devname = ptsname(ptm)) == NULL
#else
            ptsname_r(ptm, devname, sizeof(devname))
#endif
       )) {
        return throw_runtime_exception(env, "Cannot grantpt()/unlockpt()/ptsname_r() on /dev/ptmx");
    }

    // Enable UTF-8 mode and disable software flow control (XON/XOFF) 
    // to prevent Ctrl+S from accidentally freezing the terminal.
    struct termios tios;
    tcgetattr(ptm, &tios);
    tios.c_iflag |= IUTF8;
    tios.c_iflag &= ~(IXON | IXOFF);
    tcsetattr(ptm, TCSANOW, &tios);

    // Set the initial terminal window size.
    struct winsize sz = { 
        .ws_row = (unsigned short) rows, 
        .ws_col = (unsigned short) columns, 
        .ws_xpixel = (unsigned short) (columns * cell_width), 
        .ws_ypixel = (unsigned short) (rows * cell_height)
    };
    ioctl(ptm, TIOCSWINSZ, &sz);

    pid_t pid = fork();
    if (unlikely(pid < 0)) {
        return throw_runtime_exception(env, "Fork failed");
    } else if (pid > 0) {
        // --- Parent Process ---
        *pProcessId = (int) pid;
        return ptm;
    } else {
        // --- Child Process ---
        
        // Unblock signals that might have been blocked by the Java VM.
        sigset_t signals_to_unblock;
        sigfillset(&signals_to_unblock);
        sigprocmask(SIG_UNBLOCK, &signals_to_unblock, 0);

        close(ptm);
        setsid(); // Create a new session.

        int pts = open(devname, O_RDWR);
        if (pts < 0) exit(-1);

        // Redirect standard input, output, and error to the pseudo-terminal.
        dup2(pts, 0);
        dup2(pts, 1);
        dup2(pts, 2);

        // Close all other open file descriptors inherited from the parent (except 0, 1, 2).
        DIR* self_dir = opendir("/proc/self/fd");
        if (likely(self_dir != NULL)) {
            int self_dir_fd = dirfd(self_dir);
            struct dirent* entry;
            while ((entry = readdir(self_dir)) != NULL) {
                int fd = atoi(entry->d_name);
                if (fd > 2 && fd != self_dir_fd) close(fd);
            }
            closedir(self_dir);
        }

        // Setup the environment variables.
        clearenv();
        if (envp) for (; *envp; ++envp) putenv(*envp);

        // Change the working directory.
        if (chdir(cwd) != 0) {
            char* error_message;
            // Allocate error message; no need to free since we exit immediately on failure.
            if (asprintf(&error_message, "chdir(\"%s\")", cwd) == -1) error_message = "chdir()";
            perror(error_message);
            fflush(stderr);
        }
        
        // Execute the command.
        execvp(cmd, argv);
        
        // If execvp returns, it means execution failed. Print error and exit.
        char* error_message;
        if (asprintf(&error_message, "exec(\"%s\")", cmd) == -1) error_message = "exec()";
        perror(error_message);
        _exit(1);
    }
}

JNIEXPORT jint JNICALL Java_com_termux_terminal_JNI_createSubprocess(
        JNIEnv* env,
        jclass TERMUX_UNUSED(clazz),
        jstring cmd,
        jstring cwd,
        jobjectArray args,
        jobjectArray envVars,
        jintArray processIdArray,
        jint rows,
        jint columns,
        jint cell_width,
        jint cell_height)
{
    // Convert Java string array (args) to C string array (argv).
    jsize size = args ? (*env)->GetArrayLength(env, args) : 0;
    char** argv = NULL;
    if (size > 0) {
        argv = (char**) malloc((size + 1) * sizeof(char*));
        if (unlikely(!argv)) return throw_runtime_exception(env, "Couldn't allocate argv array");
        for (int i = 0; i < size; ++i) {
            jstring arg_java_string = (jstring) (*env)->GetObjectArrayElement(env, args, i);
            char const* arg_utf8 = (*env)->GetStringUTFChars(env, arg_java_string, NULL);
            if (unlikely(!arg_utf8)) return throw_runtime_exception(env, "GetStringUTFChars() failed for argv");
            argv[i] = strdup(arg_utf8);
            (*env)->ReleaseStringUTFChars(env, arg_java_string, arg_utf8);
        }
        argv[size] = NULL;
    }

    // Convert Java string array (envVars) to C string array (envp).
    size = envVars ? (*env)->GetArrayLength(env, envVars) : 0;
    char** envp = NULL;
    if (size > 0) {
        envp = (char**) malloc((size + 1) * sizeof(char *));
        if (unlikely(!envp)) {
             // Cleanup argv to prevent memory leaks if envp allocation fails.
             if(argv) { for (char** tmp = argv; *tmp; ++tmp) free(*tmp); free(argv); }
             return throw_runtime_exception(env, "malloc() for envp array failed");
        }
        for (int i = 0; i < size; ++i) {
            jstring env_java_string = (jstring) (*env)->GetObjectArrayElement(env, envVars, i);
            char const* env_utf8 = (*env)->GetStringUTFChars(env, env_java_string, 0);
            if (unlikely(!env_utf8)) return throw_runtime_exception(env, "GetStringUTFChars() failed for env");
            envp[i] = strdup(env_utf8);
            (*env)->ReleaseStringUTFChars(env, env_java_string, env_utf8);
        }
        envp[size] = NULL;
    }

    int procId = 0;
    char const* cmd_cwd = (*env)->GetStringUTFChars(env, cwd, NULL);
    char const* cmd_utf8 = (*env)->GetStringUTFChars(env, cmd, NULL);
    
    // Perform the actual subprocess creation.
    int ptm = create_subprocess(env, cmd_utf8, cmd_cwd, argv, envp, &procId, rows, columns, cell_width, cell_height);
    
    // Release Java strings properly.
    (*env)->ReleaseStringUTFChars(env, cmd, cmd_utf8);
    (*env)->ReleaseStringUTFChars(env, cwd, cmd_cwd);

    // Free allocated C arrays.
    if (argv) {
        for (char** tmp = argv; *tmp; ++tmp) free(*tmp);
        free(argv);
    }
    if (envp) {
        for (char** tmp = envp; *tmp; ++tmp) free(*tmp);
        free(envp);
    }

    // Return the process ID to the Java caller.
    int* pProcId = (int*) (*env)->GetPrimitiveArrayCritical(env, processIdArray, NULL);
    if (unlikely(!pProcId)) return throw_runtime_exception(env, "JNI call GetPrimitiveArrayCritical(processIdArray, &isCopy) failed");

    *pProcId = procId;
    (*env)->ReleasePrimitiveArrayCritical(env, processIdArray, pProcId, 0);

    return ptm;
}

JNIEXPORT void JNICALL Java_com_termux_terminal_JNI_setPtyWindowSize(JNIEnv* TERMUX_UNUSED(env), jclass TERMUX_UNUSED(clazz), jint fd, jint rows, jint cols, jint cell_width, jint cell_height)
{
    struct winsize sz = { 
        .ws_row = (unsigned short) rows, 
        .ws_col = (unsigned short) cols, 
        .ws_xpixel = (unsigned short) (cols * cell_width), 
        .ws_ypixel = (unsigned short) (rows * cell_height) 
    };
    ioctl(fd, TIOCSWINSZ, &sz);
}

JNIEXPORT void JNICALL Java_com_termux_terminal_JNI_setPtyUTF8Mode(JNIEnv* TERMUX_UNUSED(env), jclass TERMUX_UNUSED(clazz), jint fd)
{
    struct termios tios;
    tcgetattr(fd, &tios);
    if ((tios.c_iflag & IUTF8) == 0) {
        tios.c_iflag |= IUTF8;
        tcsetattr(fd, TCSANOW, &tios);
    }
}

JNIEXPORT jint JNICALL Java_com_termux_terminal_JNI_waitFor(JNIEnv* TERMUX_UNUSED(env), jclass TERMUX_UNUSED(clazz), jint pid)
{
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return -WTERMSIG(status);
    } else {
        // This case should theoretically never be reached.
        return 0;
    }
}

JNIEXPORT void JNICALL Java_com_termux_terminal_JNI_close(JNIEnv* TERMUX_UNUSED(env), jclass TERMUX_UNUSED(clazz), jint fileDescriptor)
{
    close(fileDescriptor);
}
