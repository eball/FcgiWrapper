
#ifndef __FCGIWRAPPER_H__
#define __FCGIWRAPPER_H__

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <signal.h>

#define FCGI_LISTENSOCK_FILENO 0
#define FALSE 0
#define TRUE 1

# include <sys/socket.h>
# include <sys/ioctl.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <sys/un.h>
# include <arpa/inet.h>

# include <netdb.h>

#include <sys/wait.h>

#include <vector>

namespace fcgi{

template <typename CGI_PROC>
class FcgiWrapper{
public:
    FcgiWrapper(){};
    ~FcgiWrapper(){
		_killSpawnedChildren();
	};

    int serve(const char *addr, unsigned short port, const char *unixsocket,CGI_PROC* cgiProcess, void *cgiArgv, int worker){
        int fcgi_fd = 0;
        mode_t sockmode =  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) & ~read_umask();
        
        fcgi_fd = _bindSocket(addr, port, unixsocket, sockmode, 1024);
        if(fcgi_fd == -1){
            return -1;
        }

        _spawnChild(cgiProcess, cgiArgv, fcgi_fd, worker);

        _fastCgiProcMgr(cgiProcess,cgiArgv,fcgi_fd);
        
        close(fcgi_fd);

        return 0;
    }
    
private:
    ::std::vector<pid_t> _spawnedChildren;

    mode_t read_umask(void) {
        mode_t mask = umask(0);
        umask(mask);
        return mask;
    }
	
	
    ////////////////////////////////////
    // socket functions
    //
    typedef int socklen_t;
    int _bindSocket(const char *addr, unsigned short port, const char *unixsocket, mode_t mode, int backlog) {
        int fcgi_fd, socket_type, val;

        struct sockaddr_un fcgi_addr_un;
        struct sockaddr_in fcgi_addr_in;
        struct sockaddr *fcgi_addr;

        socklen_t servlen;

        if (unixsocket) {
            memset(&fcgi_addr_un, 0, sizeof(fcgi_addr_un));

            fcgi_addr_un.sun_family = AF_UNIX;
            /* already checked in main() */
            if (strlen(unixsocket) > sizeof(fcgi_addr_un.sun_path) - 1) return -1;
            strcpy(fcgi_addr_un.sun_path, unixsocket);

            servlen = strlen(fcgi_addr_un.sun_path) + sizeof(fcgi_addr_un.sun_family);
            socket_type = AF_UNIX;
            fcgi_addr = (struct sockaddr *) &fcgi_addr_un;

            /* check if some backend is listening on the socket
            * as if we delete the socket-file and rebind there will be no "socket already in use" error
            */
            if (-1 == (fcgi_fd = socket(socket_type, SOCK_STREAM, 0))) {
                fprintf(stderr, "fcgi-wrapper: couldn't create socket: %s\n", strerror(errno));
                return -1;
            }

            if (0 == connect(fcgi_fd, fcgi_addr, servlen)) {
                fprintf(stderr, "fcgi-wrapper: socket is already in use, can't spawn\n");
                close(fcgi_fd);
                return -1;
            }

            /* cleanup previous socket if it exists */
            if (-1 == unlink(unixsocket)) {
                switch (errno) {
                case ENOENT:
                    break;
                default:
                    fprintf(stderr, "fcgi-wrapper: removing old socket failed: %s\n", strerror(errno));
                    close(fcgi_fd);
                    return -1;
                }
            }

            close(fcgi_fd);
        } else {
            memset(&fcgi_addr_in, 0, sizeof(fcgi_addr_in));
            fcgi_addr_in.sin_family = AF_INET;
            fcgi_addr_in.sin_port = htons(port);

            servlen = sizeof(fcgi_addr_in);
            socket_type = AF_INET;
            fcgi_addr = (struct sockaddr *) &fcgi_addr_in;

            if (addr == NULL) {
                fcgi_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    #ifdef HAVE_INET_PTON
            } else if (1 == inet_pton(AF_INET, addr, &fcgi_addr_in.sin_addr)) {
                /* nothing to do */
            } else {
                fprintf(stderr, "fcgi-wrapper: '%s' is not a valid IP address\n", addr);
                return -1;
    #else
            } else {
                if ((in_addr_t)(-1) == (fcgi_addr_in.sin_addr.s_addr = inet_addr(addr))) {
                    fprintf(stderr, "fcgi-wrapper: '%s' is not a valid IPv4 address\n", addr);
                    return -1;
                }
    #endif
            }
        }


        if (-1 == (fcgi_fd = socket(socket_type, SOCK_STREAM, 0))) {
            fprintf(stderr, "fcgi-wrapper: couldn't create socket: %s\n", strerror(errno));
            return -1;
        }

        val = 1;
        if (setsockopt(fcgi_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
            fprintf(stderr, "fcgi-wrapper: couldn't set SO_REUSEADDR: %s\n", strerror(errno));
            close(fcgi_fd);
            return -1;
        }

        if (-1 == bind(fcgi_fd, fcgi_addr, servlen)) {
            fprintf(stderr, "fcgi-wrapper: bind failed: %s\n", strerror(errno));
            close(fcgi_fd);
            return -1;
        }

        if (unixsocket) {
            if (-1 == chmod(unixsocket, mode)) {
                fprintf(stderr, "fcgi-wrapper: couldn't chmod socket: %s\n", strerror(errno));
                close(fcgi_fd);
                unlink(unixsocket);
                return -1;
            }
        }

        if (-1 == listen(fcgi_fd, backlog)) {
            fprintf(stderr, "fcgi-wrapper: listen failed: %s\n", strerror(errno));
            close(fcgi_fd);
            if (unixsocket) unlink(unixsocket);
            return -1;
        }

        return fcgi_fd;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // spawn functions
    //
    int _spawnChild(CGI_PROC* cgiProcess, void *cgiArgv, int fcgi_fd, int fork_count) {
        int status, rc = 0;
        struct timeval tv = { 0, 100 * 1000 };

        pid_t child;

        while (fork_count-- > 0) {

            child = fork();
			    
            switch (child) {
            case 0: {
//                int max_fd = 0;
//
//                int i = 0;
				// 恢复子进程信号
				signal(SIGTERM, SIG_DFL);
				signal(SIGCHLD, SIG_DFL);
				signal(SIGALRM, SIG_DFL);
				signal(SIGQUIT, SIG_DFL);
				signal(SIGINT, 	SIG_DFL);

                if(fcgi_fd != FCGI_LISTENSOCK_FILENO) {
                    close(FCGI_LISTENSOCK_FILENO);
                    dup2(fcgi_fd, FCGI_LISTENSOCK_FILENO);
                    close(fcgi_fd);
                }

                /* loose control terminal */
                setsid();

//                max_fd = open("/dev/null", O_RDWR);
//                if (-1 != max_fd) {
//                    if (max_fd != STDOUT_FILENO) dup2(max_fd, STDOUT_FILENO);
//                    if (max_fd != STDERR_FILENO) dup2(max_fd, STDERR_FILENO);
//                    if (max_fd != STDOUT_FILENO && max_fd != STDERR_FILENO) close(max_fd);
//                } else {
//                    fprintf(stderr, "fcgi-wrapper: couldn't open and redirect stdout/stderr to '/dev/null': %s\n", strerror(errno));
//                }
//
//                /* we don't need the client socket */
//				for (i = 3; i < max_fd; i++) {
//                    if (i != FCGI_LISTENSOCK_FILENO) close(i);
//                }

                /* fork and replace shell */
                (*cgiProcess)(cgiArgv);

                /* in nofork mode stderr is still open */
                fprintf(stderr, "fcgi-wrapper: exec failed: %s\n", strerror(errno));
                exit(errno);

                break;
            }
            case -1:
                /* error */
                fprintf(stderr, "fcgi-wrapper: fork failed: %s\n", strerror(errno));
                break;
            default:
                /* father */

                /* wait */
                select(0, NULL, NULL, NULL, &tv);

                switch (waitpid(child, &status, WNOHANG)) {
                case 0:
                    fprintf(stdout, "fcgi-wrapper: child spawned successfully: PID: %d\n", child);

                    // add pid to watch list
                    _spawnedChildren.push_back(child);
                    break;
                case -1:
                    break;
                default:
                    if (WIFEXITED(status)) {
                        fprintf(stderr, "fcgi-wrapper: child exited with: %d\n",
                            WEXITSTATUS(status));
                        rc = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        fprintf(stderr, "fcgi-wrapper: child signaled: %d\n",
                            WTERMSIG(status));
                        rc = 1;
                    } else {
                        fprintf(stderr, "fcgi-wrapper: child died somehow: exit status = %d\n",
                            status);
                        rc = status;
                    }
                }

                break;
            }
        }
        return rc;
    }

    void _killSpawnedChildren(){
        for(::std::vector<pid_t>::iterator i = _spawnedChildren.begin();
            i != _spawnedChildren.end();
            i++){
				fprintf(stderr, "fcgi-wrapper: killing child: %d\n", *i);
				if(*i > 0){
					kill(*i, SIGTERM);
				}
			}
		
		_spawnedChildren.erase(_spawnedChildren.begin(), _spawnedChildren.end());
    }


    ////////////////////////////////////////////////////////////////////////////////////////
    // cgi mgr functions
    //
    static int caughtSigTerm;
    static int caughtSigChld;
    static sigset_t signalsToBlock;
    static void FastCgiProcMgrSignalHander(int signo){
		fprintf(stdout, "fcgi-wrapper: cgi got signal %d\n", signo);
        if(signo == SIGTERM || signo == SIGINT || signo == SIGQUIT) {
            caughtSigTerm = TRUE;
        } else if(signo == SIGCHLD) {
            caughtSigChld = TRUE;
        }
    }

    static int CaughtSigTerm(void){
        int result;

        /*
        * Start of critical region for caughtSigTerm
        */
        sigprocmask(SIG_BLOCK, &signalsToBlock, NULL);
        result = caughtSigTerm;
        sigprocmask(SIG_UNBLOCK, &signalsToBlock, NULL);

        /*
        * End of critical region for caughtSigTerm
        */
        return result;
    }

    void _fastCgiProcMgr(CGI_PROC* cgiProcess, void *cgiArgv, int fcgi_fd){
        /*
        * Set up to handle SIGTERM, SIGCHLD, and SIGALRM.
        */
        sigset_t sigMask;
        sigemptyset(&signalsToBlock);
        sigaddset(&signalsToBlock, SIGTERM);
        sigaddset(&signalsToBlock, SIGCHLD);
        sigaddset(&signalsToBlock, SIGALRM);
        sigaddset(&signalsToBlock, SIGQUIT);
        sigaddset(&signalsToBlock, SIGINT);
        sigprocmask(SIG_BLOCK, NULL, &sigMask);
        sigdelset(&sigMask, SIGTERM);
        sigdelset(&sigMask, SIGCHLD);
        sigdelset(&sigMask, SIGALRM);
        sigdelset(&sigMask, SIGQUIT);
        sigdelset(&sigMask, SIGINT);
        signal(SIGTERM, FastCgiProcMgrSignalHander);
        signal(SIGCHLD, FastCgiProcMgrSignalHander);
        signal(SIGALRM, FastCgiProcMgrSignalHander);
		signal(SIGQUIT, FastCgiProcMgrSignalHander);
		signal(SIGINT, 	FastCgiProcMgrSignalHander);

		fprintf(stdout, "fcgi-wrapper: cgi manager start\n");

        for(;;){
            if(CaughtSigTerm()) {
                _killSpawnedChildren();
                break;
            }

            ::std::vector<pid_t>::iterator iter = _spawnedChildren.begin();
            int restartCount = 0;
            while(iter != _spawnedChildren.end()){
                int waitStatus;

                // check child process
                pid_t childPid = waitpid(*iter, &waitStatus, WNOHANG);
                if(childPid == -1 || childPid == 0){
                    iter++;
                }else{
                    iter = _spawnedChildren.erase(iter);
                    restartCount++;
                }
                
            } // end while

            if(restartCount > 0){
				fprintf(stdout, "fcgi-wrapper: child re-spawning %d\n", restartCount);
                
                _spawnChild(cgiProcess, cgiArgv, fcgi_fd, restartCount);
                
                /*
                * Start of critical region for caughtSigChld and caughtSigTerm.
                */
                sigprocmask(SIG_BLOCK, &signalsToBlock, NULL);
                if(caughtSigTerm) {
                    _killSpawnedChildren();
                    break;
                }
                caughtSigChld = FALSE;
                sigprocmask(SIG_UNBLOCK, &signalsToBlock, NULL);

            } 

            // sleep wait
            struct timeval tv = { 0, 100 * 1000 };
            select(0, NULL, NULL, NULL, &tv);
            
        } // end for(;;)
    }
}; // end class 


} // end namespace

#endif // __FCGIWRAPPER_H__

