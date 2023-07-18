#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wait.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include "debug.h"
#include "polya.h"

// Struct for each worker to formally layout all of the necessary information
struct probWorker {
    int parToWor[2]; // Parent writes (parToWor[1]), worker reads (parToWor[0])
    int worToPar[2]; // Parent reads (worToPar[0]), worker writes (worToPar[1])
    int currState; // Current state of worker
    int checked; // Whether or not the result was read
    int wid; // Worker id (Only used when writing debug statements)
    pid_t pid; // Process id
    struct problem *currProb; // Current problem of worker
    
} probWorker;

// Initialize variables for total workers and array of all workers
int wCurrStatus; // Status used in sigchld_handler for waitpid call
int numWorkers = 0; // Total number of workers, to be used in helper functions
struct probWorker *allWorkersArray; // Array for all of the worker structs
struct result *currResult; // Current result of the current problem
sigset_t sigsetO, sigsetN; // Old and new sigset variables

// Initialize signal-safe variables
volatile sig_atomic_t numAliveWorkers = 0; // Number of alive workers
volatile sig_atomic_t numIdleWorkers = 0; // Number of idle workers
volatile sig_atomic_t numStoppedWorkers = 0; // Number of stopped workers
volatile sig_atomic_t numSolvedProblems = 0; // Number of problems solved
volatile sig_atomic_t solvedCurrProb = 0; // Boolean of whether the current problem was solved

/**
 * Checks if any of the workers are idle
 * @return 1 if a worker is idle, 0 none of them are idle
 */
int isAnyWorkerNotIdle() {
    for(int i = 0; i < numWorkers; i++) {
        if(allWorkersArray[i].currState != WORKER_IDLE) return 1; 
    }
    return 0;
}

/**
 * Checks if all of the workers are terminated
 */
int isWorkerNotTerminated() {
    for(int k = 0; k < numWorkers; k++) {
        if(allWorkersArray[k].currState != WORKER_EXITED) return 1;
    }
    return 0;
} 

/**
 * Terminates the rest of the workers that are idle since all problems have been solved
 */ 
void terminateWorkers() {
    // Terminates all of the workers
    for(int j = 0; j < numWorkers; j++) {
        kill(allWorkersArray[j].pid, SIGCONT);
        kill(allWorkersArray[j].pid, SIGTERM);
    }
    // Waits until all of the workers are done terminating
    while(1) {
        if(!isWorkerNotTerminated()) break;
    }
}

/**
 * Signal handler for each of the workers
 */
void sigchld_handler() {
    pid_t wpid;

    // Figure out the options for waitpid
    wpid = waitpid(-1, &wCurrStatus, WNOHANG | WUNTRACED | WCONTINUED);
    while(wpid > 0) {
        // Create new worker, set new worker if pid matches pid of worker
        struct probWorker *currWor = NULL; 
        for(int a = 0; a < numWorkers; a++) {
            if(wpid == allWorkersArray[a].pid) currWor = &allWorkersArray[a];
        }

        // Exit program if pid doesn't match
        if(currWor == NULL) {  
            exit(EXIT_FAILURE);
        }

        // Check state changes
        if(WIFCONTINUED(wCurrStatus)) {
            sf_change_state(wpid, currWor->currState, WORKER_RUNNING);
            currWor->currState = WORKER_RUNNING;
        } else if(WIFSTOPPED(wCurrStatus)) {
            /* Check status of current state of worker. If worker just started, then the stop signal
            causes it to be idle. Otherwise, it will be stopped */
            if(currWor->currState == WORKER_STARTED) {
                sf_change_state(wpid, currWor->currState, WORKER_IDLE);
                currWor->currState = WORKER_IDLE;
                numIdleWorkers++;
                numAliveWorkers++;;
            } else {
                sf_change_state(wpid, currWor->currState, WORKER_STOPPED);
                currWor->currState = WORKER_STOPPED;
                numStoppedWorkers++;
            }
        } else if(WIFEXITED(wCurrStatus)) {
            sf_change_state(wpid, currWor->currState, WORKER_EXITED);
            currWor->currState = WORKER_EXITED;
        } else if(WIFSIGNALED(wCurrStatus)) {
            // Worker terminated by signal
            exit(EXIT_FAILURE);
        }
        
        // Update wpid
        wpid = waitpid(-1, &wCurrStatus, WNOHANG | WUNTRACED | WCONTINUED);
    }
}

/*
 * master
 * (See polya.h for specification.)
 */
int master(int workers) {
    // Initialize master and worker array
    sf_start();
    sigfillset(&sigsetO);
    sigfillset(&sigsetN);
    numWorkers = workers;
    allWorkersArray = malloc(sizeof(struct probWorker) *workers);

    // Create SIGCHLD handler
    signal(SIGCHLD, sigchld_handler);

    // Initialize each of the workers
    for(int i = 0; i < workers; i++) {
        struct probWorker *newWorker = &allWorkersArray[i];

        // Set all file descriptors to -1, will eventually get changed
        newWorker->parToWor[0] = -1; newWorker->parToWor[1] = -1;
        newWorker->worToPar[0] = -1; newWorker->worToPar[1] = -1;

        // Create pipes for parToWor and worToPar
        pipe(newWorker->worToPar); pipe(newWorker->parToWor);

        // Set currState to WORKER_STARTED since the program is starting the worker and currProb to NULL
        newWorker->currState = WORKER_STARTED;
        newWorker->currProb = NULL;
        newWorker->checked = 0;

        // Set wid and pid to worker
        newWorker->wid = i;
        newWorker->pid = fork();

        // Execute worker
        if(newWorker->pid == 0) {
            // Set the file descriptors in the parToWor pipe
            dup2(newWorker->parToWor[0], STDIN_FILENO);
            close(newWorker->parToWor[1]);

            // Set the file descriptors in the worToPar pipe
            dup2(newWorker->worToPar[1], STDOUT_FILENO);
            close(newWorker->worToPar[0]);

            execl("./bin/polya_worker", "polya_worker", NULL); 
        } else sf_change_state(newWorker->pid, 0, WORKER_STARTED);
    }

    // Loop until all problems are solved or all workers are not idle
    while(1) {
        while(isAnyWorkerNotIdle()) {
            // Check if the stopped workers got the result
            if(numStoppedWorkers > 0) {
                
                // Loop through each worker and see which one's have the currState of WORKER_STOPPED
                for(int i = 0; i < workers; i++) {
                    if(allWorkersArray[i].currState == WORKER_STOPPED && !solvedCurrProb 
                    && !allWorkersArray[i].checked) {
                        // Read the result to currResult 
                        currResult = malloc(sizeof(struct result));
                        FILE *outFile = fdopen(allWorkersArray[i].worToPar[0], "r");
                        
                        //read(allWorkersArray[i].worToPar[0], currResult, sizeof(struct result));
                        int c;
                        char resHeaderBuf[sizeof(struct result)];
                        for(int j = 0; j < sizeof(struct result); j++) {
                            c = fgetc(outFile);
                            if(c == EOF) { 
                                currResult = NULL; 
                                break; 
                            }
                            resHeaderBuf[j] = c;
                        }
                        memcpy(currResult, &resHeaderBuf[0], sizeof(struct result));
                        allWorkersArray[i].checked = 1;
                        
                        // If result, get contents, otherwise it has not finished yet
                        if(currResult) {
                            // Reallocate result with the new size
                            currResult = realloc(currResult, currResult->size);
                            int resDataSize = currResult->size - sizeof(struct result);

                            // If result has data size > 0, read the rest
                            //read(allWorkersArray[i].worToPar[0], &currResult->data[0], resDataSize);
                            if(resDataSize > 0) {
                                for(int j = 0; j < resDataSize; j++) {
                                    c = fgetc(outFile);
                                    if(c == EOF) break;
                                    currResult->data[j] = c;
                                }
                            }

                            // Must be called when receiving result
                            sf_recv_result(allWorkersArray[i].pid, currResult);

                            if(currResult->failed == 0) {
                                post_result(currResult, allWorkersArray[i].currProb);
                                numSolvedProblems++;
                                solvedCurrProb = 1;
                            } else {
                                free(currResult);
                                continue;
                            }  
                        } else {
                            sf_change_state(allWorkersArray[i].pid, allWorkersArray[i].currState, WORKER_IDLE);
                            allWorkersArray[i].currState = WORKER_IDLE;
                            allWorkersArray[i].currProb = NULL;
                            numStoppedWorkers--;
                            numIdleWorkers++;
                            continue;
                        }
                        
                        // Change state of the worker
                        sf_change_state(allWorkersArray[i].pid, allWorkersArray[i].currState, WORKER_IDLE);

                        // Change currProb to NULL
                        allWorkersArray[i].currProb = NULL;

                        // Change currState to idle
                        allWorkersArray[i].currState = WORKER_IDLE;

                        // Decrease and increase counts (stopped and idle respectively)
                        numStoppedWorkers--;
                        numIdleWorkers++;

                        // Free currResult as it is no longer needed
                        free(currResult);

                        // Cancel the rest of the workers
                        for(int j = 0; j < workers; j++) {
                            if(allWorkersArray[j].currState == WORKER_CONTINUED 
                            || allWorkersArray[j].currState == WORKER_RUNNING) {
                                sf_cancel(allWorkersArray[j].pid);
                                kill(allWorkersArray[j].pid, SIGHUP);
                            }
                        }
                    } else if(allWorkersArray[i].currState == WORKER_STOPPED) {
                        // Change state of workers (already done in if statement above)
                        sf_change_state(allWorkersArray[i].pid, allWorkersArray[i].currState, WORKER_IDLE);
                        allWorkersArray[i].currState = WORKER_IDLE;
                        allWorkersArray[i].currProb = NULL;
                        numStoppedWorkers--;
                        numIdleWorkers++;
                    }
                }
            }
        }    

        // Terminate if all problems solved, give next problem otherwise
        if(!isAnyWorkerNotIdle()) { 
            // Give next problem to all workers
            for(int i = 0; i < workers; i++) {
                // Get the problem variant
                struct problem *newProb = get_problem_variant(workers, i);
                if(!newProb) {
                    terminateWorkers();
                    sf_end();
                    exit(EXIT_SUCCESS);
                }

                solvedCurrProb = 0;

                // Send the problem to worker and change worker state
                sf_send_problem(allWorkersArray[i].pid, newProb);
                sf_change_state(allWorkersArray[i].pid, allWorkersArray[i].currState, WORKER_CONTINUED);
                allWorkersArray[i].currState = WORKER_CONTINUED;
                allWorkersArray[i].currProb = newProb;
                allWorkersArray[i].checked = 0;

                // Write the new problem
                FILE *inFile = fdopen(allWorkersArray[i].parToWor[1], "w");
                // write(allWorkersArray[i].parToWor[1], newProb, newProb->size);
                char wSolBuf[newProb->size];
                memcpy(&wSolBuf[0], (char *)newProb, newProb->size);
                for(int j = 0; j < newProb->size; j++) {
                    fputc(wSolBuf[j], inFile);
                }
                fflush(inFile);
                kill(allWorkersArray[i].pid, SIGCONT);
                
                // Change number of idle workers and new problem to not solved
                numIdleWorkers--;
            } 
        }
    }

    terminateWorkers();
    sf_end();
    exit(EXIT_FAILURE);
}
