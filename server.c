#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>

// --- Configuration ---
#define PORT 8080
#define ADMIN_PASSWORD "admin123"
#define CANDIDATES_FILE "candidates.txt"
#define VOTERS_FILE "voters.txt"
#define VOTED_FILE "voted.txt"
#define VOTES_FILE "votes.txt"
#define MAX_CANDIDATES 50

// --- Data Structures ---
typedef struct {
    int id;
    char name[100];
    char symbol[100];
} Candidate;

// Structure to hold the state of a connection, specifically for POST data
struct ConnectionInfo {
    struct MHD_PostProcessor *post_processor;
    char *action;
    char *aadhar;
    char *name;
    char *candidate_str;
    char *password; // Added for admin results
};

// --- Data Handling Functions ---
int load_candidates(Candidate candidates[]) {
    FILE* file = fopen(CANDIDATES_FILE, "r");
    if (!file) return 0;
    int count = 0;
    while (fscanf(file, "%d,%99[^,],%99[^\n]\n", &candidates[count].id, candidates[count].name, candidates[count].symbol) == 3 && count < MAX_CANDIDATES) {
        count++;
    }
    fclose(file);
    return count;
}

int is_voter_registered(const char* aadhar, const char* name) {
    FILE* file = fopen(VOTERS_FILE, "r");
    if (!file) return 0;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char file_aadhar[20], file_name[100];
        if (sscanf(line, "%19[^,],%99[^\n]", file_aadhar, file_name) == 2) {
            if (strcmp(aadhar, file_aadhar) == 0 && strcmp(name, file_name) == 0) {
                fclose(file);
                return 1;
            }
        }
    }
    fclose(file);
    return 0;
}

int has_voted(const char* aadhar) {
    FILE* file = fopen(VOTED_FILE, "r");
    if (!file) return 0;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        if (strcmp(aadhar, line) == 0) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

void record_vote(int candidate_id) {
    FILE* file = fopen(VOTES_FILE, "a");
    if (file) {
        fprintf(file, "%d\n", candidate_id);
        fclose(file);
    }
}

void record_voter_turnout(const char* aadhar) {
    FILE* file = fopen(VOTED_FILE, "a");
    if (file) {
        fprintf(file, "%s\n", aadhar);
        fclose(file);
    }
}

// --- POST Data Iterator ---
enum MHD_Result post_iterator(void *cls, enum MHD_ValueKind kind, const char *key,
                              const char *filename, const char *content_type,
                              const char *transfer_encoding, const char *data,
                              uint64_t off, size_t size) {
    struct ConnectionInfo *con_info = cls;

    if (size > 0) { // Only process if there is data
        if (strcmp(key, "action") == 0) con_info->action = strdup(data);
        if (strcmp(key, "aadhar") == 0) con_info->aadhar = strdup(data);
        if (strcmp(key, "name") == 0) con_info->name = strdup(data);
        if (strcmp(key, "candidate") == 0) con_info->candidate_str = strdup(data);
        if (strcmp(key, "password") == 0) con_info->password = strdup(data);
    }

    return MHD_YES;
}

// --- Request Completion Callback ---
void request_completed(void *cls, struct MHD_Connection *connection,
                       void **con_cls, enum MHD_RequestTerminationCode toe) {
    struct ConnectionInfo *con_info = *con_cls;
    if (NULL == con_info) return;

    if (con_info->post_processor) {
        MHD_destroy_post_processor(con_info->post_processor);
    }
    if (con_info->action) free(con_info->action);
    if (con_info->aadhar) free(con_info->aadhar);
    if (con_info->name) free(con_info->name);
    if (con_info->candidate_str) free(con_info->candidate_str);
    if (con_info->password) free(con_info->password);

    free(con_info);
    *con_cls = NULL;
}


// --- Main Request Handler ---
enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                          const char *url, const char *method,
                          const char *version, const char *upload_data,
                          size_t *upload_data_size, void **con_cls) {

    char response_buffer[8192] = "";
    struct MHD_Response *response;
    int ret;

    if (NULL == *con_cls) {
        struct ConnectionInfo *con_info = calloc(1, sizeof(struct ConnectionInfo));
        *con_cls = (void *)con_info;
        return MHD_YES;
    }

    struct ConnectionInfo *con_info = *con_cls;

    if (strcmp(method, "POST") == 0) {
        if (NULL == con_info->post_processor) {
            con_info->post_processor = MHD_create_post_processor(connection, 1024, post_iterator, (void *)con_info);
            if (NULL == con_info->post_processor) return MHD_NO;
        }

        if (*upload_data_size != 0) {
            MHD_post_process(con_info->post_processor, upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        } else {
            // --- Handle Vote Submission ---
            if (con_info->action && strcmp(con_info->action, "submit_vote") == 0) {
                if (!con_info->aadhar || !con_info->name || !con_info->candidate_str) {
                    sprintf(response_buffer, "<h1>Error</h1><p>Missing information.</p><a href='/'>Back</a>");
                } else if (!is_voter_registered(con_info->aadhar, con_info->name)) {
                    sprintf(response_buffer, "<h1>Validation Failed</h1><p>Your credentials do not match.</p><a href='/'>Back</a>");
                } else if (has_voted(con_info->aadhar)) {
                    sprintf(response_buffer, "<h1>Already Voted</h1><p>You have already cast your vote.</p><a href='/'>Back</a>");
                } else {
                    record_voter_turnout(con_info->aadhar);
                    record_vote(atoi(con_info->candidate_str));
                    sprintf(response_buffer, "<h1>Success!</h1><p>Your vote has been recorded.</p><a href='/'>Back</a>");
                }
            }
            // --- Handle Results Request ---
            else if (con_info->action && strcmp(con_info->action, "show_results") == 0) {
                if (!con_info->password || strcmp(con_info->password, ADMIN_PASSWORD) != 0) {
                    sprintf(response_buffer, "<h1>Access Denied</h1><p>Incorrect password.</p><a href='/'>Back</a>");
                } else {
                    // --- Tally Votes and Find Winner ---
                    Candidate candidates[MAX_CANDIDATES];
                    int num_candidates = load_candidates(candidates);
                    int *votes = calloc(num_candidates, sizeof(int));
                    int total_votes = 0;

                    FILE* file = fopen(VOTES_FILE, "r");
                    if (file) {
                        int choice;
                        while(fscanf(file, "%d", &choice) == 1) {
                            total_votes++;
                            for(int i = 0; i < num_candidates; i++) {
                                if (candidates[i].id == choice) {
                                    votes[i]++;
                                    break;
                                }
                            }
                        }
                        fclose(file);
                    }
                    
                    int max_votes = -1;
                    int winner_index = -1;
                    int is_tie = 0;
                    for (int i = 0; i < num_candidates; i++) {
                        if (votes[i] > max_votes) {
                            max_votes = votes[i];
                            winner_index = i;
                            is_tie = 0;
                        } else if (votes[i] == max_votes && max_votes > 0) {
                            is_tie = 1;
                        }
                    }

                    // --- Build Results Page ---
                    char temp_buffer[512];
                    strcat(response_buffer, "<!DOCTYPE html>... (HTML header)"); // Full header omitted for brevity
                    strcat(response_buffer, "<div class='max-w-2xl mx-auto bg-white p-8 rounded-2xl shadow-xl'>");
                    strcat(response_buffer, "<h1 class='text-3xl font-bold text-center mb-8'>Election Results</h1>");
                    for (int i=0; i < num_candidates; i++) {
                        sprintf(temp_buffer, "<p class='text-lg p-3 bg-gray-50 rounded-md mb-2'>%s: <span class='font-bold'>%d votes</span></p>", candidates[i].name, votes[i]);
                        strcat(response_buffer, temp_buffer);
                    }
                    sprintf(temp_buffer, "<hr class='my-4'><p class='text-xl font-semibold'>Total Votes Cast: %d</p>", total_votes);
                    strcat(response_buffer, temp_buffer);

                    if (winner_index != -1 && !is_tie) {
                        sprintf(temp_buffer, "<h2 class='text-2xl font-bold text-green-600 mt-4'>Winner: %s</h2>", candidates[winner_index].name);
                    } else if (is_tie) {
                        sprintf(temp_buffer, "<h2 class='text-2xl font-bold text-blue-600 mt-4'>Result is a Tie!</h2>");
                    } else {
                        sprintf(temp_buffer, "<h2 class='text-2xl font-bold text-gray-600 mt-4'>No votes cast yet.</h2>");
                    }
                    strcat(response_buffer, temp_buffer);
                    strcat(response_buffer, "<br><a href='/'>Back to Portal</a></div>");
                    free(votes);
                }
            }

            response = MHD_create_response_from_buffer(strlen(response_buffer), response_buffer, MHD_RESPMEM_MUST_COPY);
            ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);
            return ret;
        }
    }

    if (strcmp(method, "GET") == 0) {
        char temp_buffer[1024];
        
        strcat(response_buffer, "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>Voting Portal</title><script src='https://cdn.tailwindcss.com'></script></head><body class='bg-gray-100 p-8'>");
        strcat(response_buffer, "<div class='max-w-2xl mx-auto bg-white p-8 rounded-2xl shadow-xl'>");
        
        // Voter Form
        strcat(response_buffer, "<h1 class='text-3xl font-bold text-center mb-8'>Online Voting Portal</h1>");
        strcat(response_buffer, "<form action='/' method='POST'>");
        strcat(response_buffer, "<input type='hidden' name='action' value='submit_vote'>");
        // ... (rest of voter form)
        strcat(response_buffer, "<div class='mb-4'><label class='block font-medium'>Aadhar Number</label><input name='aadhar' class='w-full border rounded p-2 mt-1' required></div>");
        strcat(response_buffer, "<div class='mb-6'><label class='block font-medium'>Full Name</label><input name='name' class='w-full border rounded p-2 mt-1' required></div>");
        strcat(response_buffer, "<div class='mb-6'><label class='block font-medium mb-2'>Select a Candidate</label>");
        Candidate candidates[MAX_CANDIDATES];
        int num_candidates = load_candidates(candidates);
        for(int i = 0; i < num_candidates; i++) {
            sprintf(temp_buffer, "<div class='flex items-center p-3 mb-2 bg-gray-50 rounded-lg border'><input type='radio' name='candidate' value='%d' id='cand%d' class='h-4 w-4'><label for='cand%d' class='ml-3'> %s (%s)</label></div>", candidates[i].id, candidates[i].id, candidates[i].id, candidates[i].name, candidates[i].symbol);
            strcat(response_buffer, temp_buffer);
        }
        strcat(response_buffer, "</div>");
        strcat(response_buffer, "<button type='submit' class='w-full bg-blue-600 text-white p-3 rounded-lg mt-4 hover:bg-blue-700'>Submit Vote</button></form>");
        
        // Admin Form
        strcat(response_buffer, "<hr class='my-8'>");
        strcat(response_buffer, "<h2 class='text-2xl font-semibold text-center mb-6'>Admin Panel</h2>");
        strcat(response_buffer, "<form action='/' method='POST'>");
        strcat(response_buffer, "<input type='hidden' name='action' value='show_results'>");
        strcat(response_buffer, "<div class='mb-4'><label class='block font-medium'>Admin Password</label><input type='password' name='password' class='w-full border rounded p-2 mt-1' required></div>");
        strcat(response_buffer, "<button type='submit' class='w-full bg-indigo-600 text-white p-3 rounded-lg hover:bg-indigo-700'>View Results</button></form>");
        
        strcat(response_buffer, "</div></body></html>");

        response = MHD_create_response_from_buffer(strlen(response_buffer), (void *)response_buffer, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", "text/html");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }

    return MHD_NO;
}

int main() {
    struct MHD_Daemon *daemon;
    
    FILE *fp;
    fp = fopen(CANDIDATES_FILE, "a"); if (fp) fclose(fp);
    fp = fopen(VOTERS_FILE, "a"); if (fp) fclose(fp);
    fp = fopen(VOTED_FILE, "a"); if (fp) fclose(fp);
    fp = fopen(VOTES_FILE, "a"); if (fp) fclose(fp);

    daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL,
                              &request_handler, NULL, 
                              MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL, 
                              MHD_OPTION_END);
    if (NULL == daemon) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }

    printf("Server is running on http://localhost:%d\n", PORT);
    printf("Press Enter to quit...\n");
    getchar();

    MHD_stop_daemon(daemon);
    return 0;
}
