#include <stdlib.h>

#include "debug.h"
#include "polya.h"
#include "signal.h"
#include "unistd.h"
#include "stdio.h"
#include "string.h"

/* Important Note: Variables used for communication between the handler and the main program should
generally be declared volatile so that a handler will always see values that
are up-to-date. (3 variables are volatile: sighupT, sigtermT, sigcontT) */

/****** Initialize flags and handlers ******/
volatile sig_atomic_t sighupT = 0;
void sighup_handler() {
    // Set flag to 1, so program knows the current state
    sighupT = 1;
}

void sigterm_handler() {
    // Set flag to 1, so program knows the current state
    exit(EXIT_SUCCESS);
}

volatile sig_atomic_t sigcontT = 0;
void sigcont_handler() {
    // Set flag to 1, so program knows the current state
    sigcontT = 1;
}

/*
 * worker
 * (See polya.h for specification.)
 */
int worker(void) {
    /****** SETTING UP SIGHUP AND SIGTERM HANDLERS ******/
    signal(SIGHUP, sighup_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGCONT, sigcont_handler);

    /****** SIGCONT SIGNAL SENT TO WORKER ******/
    // Every iteration automatically sends SIGSTOP signal until told otherwise
    while(1) {
        // Send SIGSTOP signal
        raise(SIGSTOP);

        // Check if SIGTERM was called
        if(sigcontT) {
            // Find size of problem
            struct problem *thisProblem;
            int problemSize = sizeof(struct problem);
            thisProblem = malloc(problemSize);
            if(thisProblem == NULL) abort();

            // Read contents into the thisProblem struct
            int c;
            char headerBuf[sizeof(struct problem)];
            for(int i = 0; i < sizeof(struct problem); i++) {
                c = fgetc(stdin);
                if(c == EOF) break;
                headerBuf[i] = c;
            }
            memcpy(thisProblem, &headerBuf[0], sizeof(struct problem));

            // Take the size of the thisProblem struct to reallocate storage
            thisProblem = realloc(thisProblem, thisProblem->size);
            if(thisProblem == NULL) abort();
            int thisProblemDataSize = thisProblem->size - problemSize;
            
            // If thisProblemDataSize > 0, read the contents of the problem
            if(thisProblemDataSize > 0) {
                for(int i = 0; i < thisProblemDataSize; i++) {
                    c = fgetc(stdin);
                    if(c == EOF) break;
                    thisProblem->data[i] = c;
                }
            }
            
            // Get the result of the problem and size of result
            struct result *thisSolution;
            int solutionSize = sizeof(struct result);
            thisSolution = solvers[thisProblem->type].solve(thisProblem, &sighupT);

            // Write the results of the solution to the pipe
            if(thisSolution) {
                // Write the result
                char solBuf[thisSolution->size];
                memcpy(&solBuf[0], (char *)thisSolution, thisSolution->size);
                for(int i = 0; i < thisSolution->size; i++) {
                    fputc(solBuf[i], stdout);
                }
                
            } else {
                // Allocate space for the struct since one was not made from the solver
                thisSolution = malloc(solutionSize);
                if(thisSolution == NULL) abort();

                // Solution size must be 16-byte aligned
                thisSolution->size = solutionSize;

                // Solution failed attribute is non-zero if it failed 
                thisSolution->failed = 1;

                // Write the result
                char solBuf[solutionSize];
                memcpy(&solBuf[0], (char *)thisSolution, thisSolution->size);
                for(int i = 0; i < thisSolution->size; i++) {
                    fputc(solBuf[i], stdout);
                }
            }
            // Call fflush once all of the data has been read and written
            fflush(stdout);
            
            // Set sighupT flag back to 0 to avoid future false cancellation
            sighupT = 0;

            // Set sigcontT flag back to 0 to avoid future false continuation
            sigcontT = 0;

            // Free the problem and result pointers as they are not needed any longer
            free(thisProblem);
            free(thisSolution);
        }

    }
    
    return EXIT_SUCCESS;
}
