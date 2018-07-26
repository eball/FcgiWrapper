/** \file __FILE__
 * fast cgi framework.
 * run as single thread and multi-process mode
 *
 * \author liuyu
 **/
#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fstream>
#include <fcgio.h>

#include "FcgiWrapper.h"


static void show_help () {
    printf(
        "Usage: demo.fcgi [options] \n" \
        "\n" \
        "Options:\n" \
        " -a <address>   bind to IPv4 address (defaults to 0.0.0.0)\n" \
        " -p <port>      bind to TCP-port (defaults to 9000)\n" \
        " -s <path>      bind to Unix domain socket\n" \
        " -F <children>  number of children to fork (default 1)\n" \
        " -?, -h         show this help\n" 
    );
}


void demo(void* arg){
    FCGX_Request request;    
    FCGX_Init();    
    FCGX_InitRequest(&request, 0, 0);
    while (FCGX_Accept_r(&request) == 0){
        fcgi_streambuf cin_fcgi_streambuf(request.in);        
        fcgi_streambuf cout_fcgi_streambuf(request.out);        
        fcgi_streambuf cerr_fcgi_streambuf(request.err);        
        ::std::cin.rdbuf(&cin_fcgi_streambuf);        
        ::std::cout.rdbuf(&cout_fcgi_streambuf);        
        ::std::cerr.rdbuf(&cerr_fcgi_streambuf);
        
		::std::cout<<"Status: 200"<<::std::endl<<::std::endl<<"Hello World"<<::std::endl;
	}
} // end function demo

typedef void (*CGIApp)(void*);


namespace fcgi{
template<>
int FcgiWrapper< CGIApp >::caughtSigTerm = FALSE;
template<>
int FcgiWrapper< CGIApp >::caughtSigChld = FALSE;

#if __APPLE__
template<>
sigset_t FcgiWrapper< CGIApp >::signalsToBlock = SIGTERM;
#else
template<>
sigset_t FcgiWrapper< CGIApp >::signalsToBlock ={{0}};
#endif

}
    
int main(int argc, char **argv)
{
    char* addr = NULL;
    char *endptr = NULL;
    char* unixsocket = NULL;
    int o,
        port = 9000,    // default port
        fork_count = 1; // default childs
    
    while (-1 != (o = getopt(argc, argv, "?ha:p:F:s:"))) {
        switch(o) {
        case 'a': addr = optarg;/* ip addr */ break;
        case 'p': port = strtol(optarg, &endptr, 10);/* port */
            if (*endptr) {
                fprintf(stderr, "cpe: invalid port: %u\n", (unsigned int) port);
                return -1;
            }
            break;
        case 'F': fork_count = strtol(optarg, NULL, 10);/*  */ break;
        case 's': unixsocket = optarg; /* unix-domain socket */ break;
        case '?':
        case 'h': show_help(); return 0;
        default:
            show_help();
            return -1;
        }
    }
    
    ::fcgi::FcgiWrapper< CGIApp > wrapper;
    
	CGIApp app = demo;
    wrapper.serve(addr, port, unixsocket, &app, NULL, fork_count);
    
    return 0;
}

