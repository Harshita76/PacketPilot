#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>

// Defining the maximum allowed constant values
#define MAX_TRUCKS 250
#define TRUCK_MAX_CAP 20
#define MAX_NEW_REQUESTS 50
#define MAX_TOTAL_PACKAGES 5000

// Defining the structs

// Struct for package arrival request
typedef struct PackageRequest {
    int packageId;
    int pickup_x;
    int pickup_y;
    int dropoff_x;
    int dropoff_y;
    int arrival_turn;
    int expiry_turn;
} PackageRequest;


// Struct for updates in the shared memory, which depicts the turn change
typedef struct TurnChangeResponse {
    long mtype;
    int turnNumber;
    int newPackageRequestCount;
    int errorOccured;
    int finished;
} TurnChangeResponse;


// Struct for the shared memory
typedef struct MainSharedMemory {
    char authStrings[MAX_TRUCKS][TRUCK_MAX_CAP + 1];
    char truckMovementInstructions[MAX_TRUCKS];
    int pickUpCommands[MAX_TRUCKS];
    int dropOffCommands[MAX_TRUCKS];
    int truckPositions[MAX_TRUCKS][2];
    int truckPackageCount[MAX_TRUCKS];
    int truckTurnsInToll[MAX_TRUCKS];
    PackageRequest newPackageRequests[MAX_NEW_REQUESTS];
    int packageLocations[MAX_TOTAL_PACKAGES][2];
} MainSharedMemory;


// Struct to request for a change in turn
typedef struct TurnReadyRequest {
    long mtype;
} TurnReadyRequest;


// Strcuct to store the state of each truck in the grid
typedef struct TruckTask {
    int truckId;
    int packageId;
    PackageRequest pkg;
    int state;
    long long priority;
} TruckTask;


// Structs to communicate with the helpers : 
typedef struct SolverRequest {
    long mtype;
    int truckNumber;
    char authStringGuess[TRUCK_MAX_CAP + 1];
} SolverRequest;

typedef struct SolverResponse {
    long mtype;
    int guessIsCorrect;
} SolverResponse;



// Function prototype
void generateAllPermutations(
    char* str, int length, int pos, char directions[], int* found, 
    int solverId, int truckId, int maxAttempts, int* attemptCount
);


// Defining the global variables
int N, D, S, T, B;
int mainMsgQId;
int solverMsgQIds[MAX_TRUCKS];
TruckTask tasks[MAX_TRUCKS];
PackageRequest pendingPackages[MAX_TOTAL_PACKAGES];
int pendingCount = 0;

// Connect to the shared memory
key_t shmKey, mainMsgQKey;
key_t solverKeys[MAX_TRUCKS];
MainSharedMemory *shmPtr;


// calculate the manhattan distance between any two cells in the grid
int manhattan(int x1, int y1, int x2, int y2) {
    int x = abs(x1 - x2);
    int y = abs(y1 - y2);
    int manh_dist = x + y;
    return manh_dist;
}


// Connect to shared memory and message queues
void initializeConnections() {
    // attaching to shared memory
    int shmId = shmget(shmKey, sizeof(MainSharedMemory), 0666);
    shmPtr = (MainSharedMemory *)shmat(shmId, NULL, 0);
    
    // connect to main message queue
    mainMsgQId = msgget(mainMsgQKey, 0666);
    
    // connect to all solver message queues
    for (int i = 0; i < S; i++) {
        solverMsgQIds[i] = msgget(solverKeys[i], 0666);
    }
}


// Get optimal direction between two points
char getDirection(int x_initial, int y_initial, int x_final, int y_final) {
    
    int x_dir = x_final - x_initial;
    int y_dir = y_final - y_initial;
    
    if (abs(x_dir) > abs(y_dir)) {
        if(x_dir > 0){
            return 'r';
        }
        else{
            return 'l';
        }
    } 
    else if (y_dir != 0) {
        if(y_dir > 0){
            return 'd';
        }
        else{
            return 'u';
        }
    }
    else{
        return 's';
    }
}



// Guess authorization string for a truck
int guessAuthString(int solverId, int truckId, int length) {

    SolverRequest req;
    SolverResponse resp;
    
    // Details about the truck for which guess is being made
    req.truckNumber = truckId;
    req.mtype = 2;

    msgsnd(solverMsgQIds[solverId], &req, sizeof(SolverRequest) - sizeof(long), 0);
    
    char directions[] = {'u', 'd', 'l', 'r'};
    char guess[TRUCK_MAX_CAP + 1];
    
    // For strings of length less than 3
    if (length <= 3) {
        int maxCombinations = 1;
        
        for (int i = 0; i < length; i++){ 
            maxCombinations *= 4;
        }
        
        int found = 0;
        int attemptCount = 0;

        generateAllPermutations(guess, length, 0, directions, &found, solverId, truckId, maxCombinations, &attemptCount);
        
        if (found){
            return 1;
        } 
    }
    
    // random guessing for longer strings
    int maxAttempts = 5000;
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        for (int i = 0; i < length; i++) {
            guess[i] = directions[rand() % 4];
        }
        guess[length] = '\0';
        
        // sending guess to solver
        req.mtype = 3;
        strcpy(req.authStringGuess, guess);
        msgsnd(solverMsgQIds[solverId], &req, sizeof(SolverRequest) - sizeof(long), 0);
        msgrcv(solverMsgQIds[solverId], &resp, sizeof(SolverResponse) - sizeof(long), 4, 0);
        
        if (resp.guessIsCorrect) {
            strcpy(shmPtr->authStrings[truckId], guess);
            return 1;
        }
    }
    
    // failed to guess the auth string
    return 0;
}



// Generate all possible combinations for the auth string recursively
void generateAllPermutations(char* str, int length, int pos, char directions[], int* found, 
    int solverId, int truckId, int maxAttempts, int* attemptCount) {

    // Stop if already found or max attempts reached
    if (*found || *attemptCount >= maxAttempts){
        return;
    }
    
    // Base case : 
    if (pos == length) {
        str[length] = '\0';
        (*attemptCount) += 1;
        
        SolverRequest req;
        SolverResponse resp;
        
        req.truckNumber = truckId;
        req.mtype = 3;
        strcpy(req.authStringGuess, str);
        
        msgsnd(solverMsgQIds[solverId], &req, sizeof(SolverRequest) - sizeof(long), 0);
        msgrcv(solverMsgQIds[solverId], &resp, sizeof(SolverResponse) - sizeof(long), 4, 0);
        
        // check validity of guess
        if (resp.guessIsCorrect) {
            strcpy(shmPtr->authStrings[truckId], str);
            *found = 1;
        }

        return;
    }
    
    // Recursively guess the auth string, trying all directions
    for (int i = 0; i < 4 && !(*found); i++) {
        str[pos] = directions[i];
        generateAllPermutations(str, length, pos + 1, directions, found, solverId, truckId, maxAttempts, attemptCount);
    }
}



// Calculate priority score for package assignment
long long calculatePriority(PackageRequest pkg, int truckX, int truckY, int currentTurn, int truckPackageCount) {
    
    // Calculate distances between package pickup and drop
    int distToPickup = manhattan(truckX, truckY, pkg.pickup_x, pkg.pickup_y);
    int distToDeliver = manhattan(pkg.pickup_x, pkg.pickup_y, pkg.dropoff_x, pkg.dropoff_y);
    int totalDist = distToPickup + distToDeliver;
    
    // time slack
    int turnsLeft = pkg.expiry_turn - currentTurn;
    int slack = turnsLeft - totalDist;
    
    long long priority = 0;
    
    // packages which will expire soon or expired
    if (slack < 0) {
        long long a = 100000000LL;
        long long b = 10000000LL;
        priority = -a + ( (long long)slack * b );
    } 
    // packages which very close deadline
    else if (slack < 3) {
        long long a = 10000000LL;
        long long b = 100000LL;
        long long c = 1000000LL;
        priority = -a - ( (long long)totalDist * b ) + ( (long long)slack * c ) ;
    } 
    // packages with tight deadline
    else if (slack < 8) {
        long long a = 1000000LL;
        long long b = 10000LL;
        long long c = 100000LL;
        priority = -a - ( (long long)totalDist * b ) + ( (long long)slack * c ) ;
    } 
    // normal packages, with reasonable time
    else {
        long long a = 1000LL;
        long long b = 50LL;
        priority = -(long long)totalDist * a  - ( (long long)turnsLeft * b ) ;
    }
    
    // penalize far pickups
    priority -= (long long)distToPickup * 200LL;
    
    // penalize if truck almost full
    if (truckPackageCount > 15) {
        priority -= 100000LL;
    }
    
    return priority;
}


// assign packages to trucks
void assignOptimalPackages(int currentTurn) {
    
    int assignmentMade = 1;
    
    while (assignmentMade) {
        
        assignmentMade = 0;
        
        // assign package to each free truck
        for (int i = 0; i < D; i++) {
            
            // skip if truck has task
            if (tasks[i].packageId != -1){
                continue;
            }
            
            // skip if truck in toll booth
            if (shmPtr->truckTurnsInToll[i] > 0){
                continue;
            }
            
            // skip if truck full
            if (shmPtr->truckPackageCount[i] >= TRUCK_MAX_CAP){
                continue;
            }
            
            int bestPkg = -1;
            long long bestPriority = LLONG_MIN;
            
            // finding best package for the current truck
            for (int j = 0; j < pendingCount; j++) {
                
                if (pendingPackages[j].packageId == -1){
                    continue;
                }
                
                int assigned = 0;
                for (int k = 0; k < D; k++) {
                    if (tasks[k].packageId == pendingPackages[j].packageId) {
                        assigned = 1;
                        break;
                    }
                }
                
                // skip if package already assigned
                if (assigned){
                    continue;
                }
                
                // priority for truck - package pair
                long long priority = calculatePriority(
                    pendingPackages[j],  shmPtr->truckPositions[i][0], 
                    shmPtr->truckPositions[i][1], currentTurn, shmPtr->truckPackageCount[i]
                );
                
                // Track best package
                if (priority > bestPriority) {
                    bestPriority = priority;
                    bestPkg = j;
                }
            }
            
            // assign best package for the current truck
            if (bestPkg != -1) {
                tasks[i].truckId = i;
                tasks[i].packageId = pendingPackages[bestPkg].packageId;
                tasks[i].pkg = pendingPackages[bestPkg];
                tasks[i].state = 0;
                tasks[i].priority = bestPriority;
                assignmentMade = 1;
            }
        }
    }
}

// pick up packages at current location
void optimizeMultiplePickups(int currentTurn) {
    
    for (int i = 0; i < D; i++) {
        if (tasks[i].packageId != -1){
            continue;
        }
        
        if (shmPtr->truckTurnsInToll[i] > 0){
            continue;
        }
        
        // skip if truck almost full
        if (shmPtr->truckPackageCount[i] >= TRUCK_MAX_CAP - 2){
            continue;
        }
        
        int ty = shmPtr->truckPositions[i][1];
        int tx = shmPtr->truckPositions[i][0];
        
        // Check if any package is at truck's current location
        for (int j = 0; j < pendingCount; j++) {
            
            if (pendingPackages[j].packageId == -1){
                continue;
            }
            
            int assigned = 0;
            for (int k = 0; k < D; k++) {
                if (tasks[k].packageId == pendingPackages[j].packageId) {
                    assigned = 1;
                    break;
                }
            }
            if (assigned){
                continue;
            }
            
            // if package is at current location
            if (tx == pendingPackages[j].pickup_x && ty == pendingPackages[j].pickup_y) {
                
                int turnsLeft = pendingPackages[j].expiry_turn - currentTurn;
                int distToDeliver = manhattan(tx, ty, pendingPackages[j].dropoff_x, pendingPackages[j].dropoff_y);
                
                // pickup package only if time left
                if (turnsLeft > distToDeliver + 3) {
                    tasks[i].truckId = i;
                    tasks[i].packageId = pendingPackages[j].packageId;
                    tasks[i].pkg = pendingPackages[j];
                    tasks[i].state = 0;
                    break;
                }
            }
        }
    }
}


// Execute movements and pickups/dropoffs for current turn
void executeTurn(int turnNumber) {
    
    for (int i = 0; i < D; i++) {
        shmPtr->pickUpCommands[i] = -1;
        shmPtr->dropOffCommands[i] = -1;
        shmPtr->truckMovementInstructions[i] = 's';
        shmPtr->authStrings[i][0] = '\0';
    }
    
    for (int i = 0; i < D; i++) {
        
        if (shmPtr->truckTurnsInToll[i] > 0){
            continue;
        }
        
        if (tasks[i].packageId == -1){
            continue;
        }
        
        int ty = shmPtr->truckPositions[i][1];
        int tx = shmPtr->truckPositions[i][0];
        
        // going to pickup location
        if (tasks[i].state == 0) {
            
            if (tx == tasks[i].pkg.pickup_x && ty == tasks[i].pkg.pickup_y) {
                shmPtr->pickUpCommands[i] = tasks[i].packageId;
                tasks[i].state = 1;
            } 
            else {
                char dir = getDirection(tx, ty, tasks[i].pkg.pickup_x, tasks[i].pkg.pickup_y);
                shmPtr->truckMovementInstructions[i] = dir;
                
                int len = shmPtr->truckPackageCount[i];

                if (len > 0) {
                    int dist = i % S;
                    guessAuthString(dist, i, len);
                }
            }
        } 
        // going to dropoff location
        else if (tasks[i].state == 1) {
            if (tx == tasks[i].pkg.dropoff_x && ty == tasks[i].pkg.dropoff_y) {
                shmPtr->dropOffCommands[i] = tasks[i].packageId;
                
                for (int j = 0; j < pendingCount; j++) {
                    if (pendingPackages[j].packageId == tasks[i].packageId) {
                        pendingPackages[j].packageId = -1;
                        break;
                    }
                }
                
                tasks[i].packageId = -1;
                tasks[i].state = 0;
            } 
            // move to dropoff location
            else {
                char dir = getDirection(tx, ty, tasks[i].pkg.dropoff_x, tasks[i].pkg.dropoff_y);
                shmPtr->truckMovementInstructions[i] = dir;
                
                int len = shmPtr->truckPackageCount[i];

                if (len > 0) {
                    int dist = i % S;
                    guessAuthString(dist, i, len);
                }
            }
        }
    }
}


// Read the input.txt file
void readInput() {
    FILE *fp = fopen("input.txt", "r");
    // size of grid
    fscanf(fp, "%d", &N);
    // number of trucks
    fscanf(fp, "%d", &D);
    // number of solvers
    fscanf(fp, "%d", &S);
    // turn number of last request
    fscanf(fp, "%d", &T);
    // number of toll booths
    fscanf(fp, "%d", &B);
    // key for shared memory
    fscanf(fp, "%d", &shmKey);
    // key for main message queue
    fscanf(fp, "%d", &mainMsgQKey);
    // each loop containing the key for message queue of a solver
    for (int i = 0; i < S; i++) {
        fscanf(fp, "%d", &solverKeys[i]);
    }
    fclose(fp);
}


int main() {
    // random seed
    srand(time(NULL));

    // read input file
    readInput();

    // connect to shared memory and queues
    initializeConnections();
    
    // Initialize all truck tasks as empty
    for (int i = 0; i < D; i++) {
        tasks[i].packageId = -1;
        tasks[i].state = 0;
    }
    
    TurnChangeResponse turnResp;
    TurnReadyRequest turnReq;
    turnReq.mtype = 1;
    
    // Main loop
    while (1) {
        
        // turn change update from helper
        msgrcv(mainMsgQId, &turnResp, sizeof(TurnChangeResponse) - sizeof(long), 2, 0);
        
        // check if eneded
        if (turnResp.errorOccured || turnResp.finished) {
            break;
        }
        
        // new package requests to pending list
        for (int i = 0; i < turnResp.newPackageRequestCount; i++) {
            pendingPackages[pendingCount++] = shmPtr->newPackageRequests[i];
        }
        
        optimizeMultiplePickups(turnResp.turnNumber);
        assignOptimalPackages(turnResp.turnNumber);
        executeTurn(turnResp.turnNumber);
        
        // send turn ready message to helper
        msgsnd(mainMsgQId, &turnReq, sizeof(TurnReadyRequest) - sizeof(long), 0);
    }
    
    // Cleanup
    shmdt(shmPtr);
    
    return 0;
}