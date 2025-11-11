#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <time.h>

#define PORT 8080

// --- File Paths ---
#define CANDIDATES_FILE "candidates.txt"
#define VOTERS_FILE "voters.txt"
#define VOTED_FILE "voted.txt"
#define VOTES_FILE "votes.txt"
#define ADMIN_PASS "admin123"

// --- Data Structures ---
typedef struct {
    int id;
    char name[100];
    char imageUrl[256];
    int votes;
} Candidate;

typedef struct {
    char aadhar[20];
    char name[100];
} Voter;

// Represents the state of a single connection
struct connection_info_struct {
    int connection_complete;
    struct MHD_PostProcessor *postprocessor;
    char action[50];
    char aadhar[20];
    char name[100];
    char candidate_str[10];
    char password[50];
};

// --- Global Data ---
Candidate candidates[20];
int num_candidates = 0;

// --- Utility Functions ---

void load_candidates() {
    FILE *file = fopen(CANDIDATES_FILE, "r");
    if (!file) {
        perror("Could not open candidates file");
        return;
    }
    printf("\n--- Loading Candidates ---\n");
    num_candidates = 0;
    while (num_candidates < 20 && fscanf(file, "%d,%99[^,],%255[^\n]\n", &candidates[num_candidates].id, candidates[num_candidates].name, candidates[num_candidates].imageUrl) == 3) {
        // *** DEBUG PRINTF ADDED HERE ***
        printf("Loaded Candidate ID: %d, Name: %s, URL: %s\n", 
               candidates[num_candidates].id, 
               candidates[num_candidates].name, 
               candidates[num_candidates].imageUrl);
        
        candidates[num_candidates].votes = 0;
        num_candidates++;
    }
    printf("--- Finished loading %d candidates ---\n\n", num_candidates);
    fclose(file);
}

int is_voter_registered(const char* aadhar, const char* name) {
    FILE* file = fopen(VOTERS_FILE, "r");
    if (!file) return 0;
    Voter v;
    while (fscanf(file, "%19[^,],%99[^\n]\n", v.aadhar, v.name) == 2) {
        if (strcmp(v.aadhar, aadhar) == 0 && strcmp(v.name, name) == 0) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

int has_voted(const char* aadhar) {
    FILE* file = fopen(VOTED_FILE, "r");
    if (!file) return 0;
    char line[20];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        if (strcmp(line, aadhar) == 0) {
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

void get_vote_counts() {
    for (int i = 0; i < num_candidates; i++) {
        candidates[i].votes = 0;
    }
    FILE* file = fopen(VOTES_FILE, "r");
    if (!file) return;
    int candidate_id;
    while (fscanf(file, "%d", &candidate_id) == 1) {
        for (int i = 0; i < num_candidates; i++) {
            if (candidates[i].id == candidate_id) {
                candidates[i].votes++;
                break;
            }
        }
    }
    fclose(file);
}

void generate_results_svg(char *buffer, size_t buffer_size) {
    get_vote_counts();
    int max_votes = 0;
    for (int i = 0; i < num_candidates; i++) {
        if (candidates[i].votes > max_votes) {
            max_votes = candidates[i].votes;
        }
    }
    if (max_votes == 0) max_votes = 1;

    int chart_width = 500;
    int bar_height = 30;
    int bar_spacing = 10;
    int chart_height = num_candidates * (bar_height + bar_spacing);

    char svg_buffer[4096] = {0};
    char temp_buffer[512];

    sprintf(svg_buffer, "<svg width='%d' height='%d' xmlns='http://www.w3.org/2000/svg' font-family='Inter, sans-serif'>", chart_width, chart_height);

    for (int i = 0; i < num_candidates; i++) {
        int bar_width = (int)(((float)candidates[i].votes / max_votes) * (chart_width - 150));
        int y_pos = i * (bar_height + bar_spacing);

        sprintf(temp_buffer, "<g><title>%s: %d votes</title><rect y='%d' width='%d' height='%d' fill='#3B82F6' rx='4'></rect>", candidates[i].name, candidates[i].votes, y_pos, bar_width, bar_height);
        strcat(svg_buffer, temp_buffer);
        sprintf(temp_buffer, "<text x='%d' y='%d' dy='20' fill='#1F2937' font-size='14' font-weight='500'>%s</text>", bar_width + 10, y_pos, candidates[i].name);
        strcat(svg_buffer, temp_buffer);
        if (bar_width > 20) {
            sprintf(temp_buffer, "<text x='5' y='%d' dy='20' fill='white' font-weight='bold' font-size='12'>%d</text>", y_pos, candidates[i].votes);
            strcat(svg_buffer, temp_buffer);
        }
        strcat(svg_buffer, "</g>");
    }

    strcat(svg_buffer, "</svg>");
    strncpy(buffer, svg_buffer, buffer_size - 1);
}

// --- HTML Generation ---

const char* generate_html_shell(const char* title, const char* body) {
    static char page[16384];
    sprintf(page,
        "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>%s</title><script src='https://cdn.tailwindcss.com'></script>"
        "<link href='https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap' rel='stylesheet'>"
        "<style>body { font-family: 'Inter', sans-serif; }</style></head>"
        "<body class='bg-gray-100 flex items-center justify-center min-h-screen'>%s</body></html>",
        title, body);
    return page;
}

const char* generate_message_page(const char* title, const char* message, int is_success) {
    char body[1024];
    sprintf(body,
        "<div class='bg-white rounded-2xl shadow-xl p-8 max-w-lg text-center'>"
        "<h1 class='text-3xl font-bold %s mb-4'>%s</h1>"
        "<p class='text-gray-700 text-lg'>%s</p>"
        "<div class='mt-8'><a href='/' class='text-blue-600 hover:underline'>Go Back</a></div></div>",
        is_success ? "text-green-600" : "text-red-600", title, message);
    return generate_html_shell(title, body);
}

const char *generate_voting_page() {
    char body[8192];
    char candidates_html[4096] = "";
    char temp_buffer[1024];

    for (int i = 0; i < num_candidates; i++) {
        // More robustly build the string to avoid buffer overflows with long URLs
        snprintf(temp_buffer, sizeof(temp_buffer),
            "<label for='cand%d' class='flex items-center p-4 bg-gray-50 rounded-lg border cursor-pointer has-[:checked]:bg-blue-50 has-[:checked]:border-blue-400'>"
            "<img src='%s' alt='%s' class='w-16 h-16 rounded-full object-cover mr-4'>"
            "<div class='flex-grow'><span class='text-lg font-medium text-gray-900'>%s</span></div>"
            "<input id='cand%d' name='candidate' type='radio' value='%d' class='h-5 w-5 text-blue-600 border-gray-300 focus:ring-blue-500' required>"
            "</label>",
            candidates[i].id, candidates[i].imageUrl, candidates[i].name, candidates[i].name, candidates[i].id, candidates[i].id);
        
        // Check if there is enough space before concatenating
        if (strlen(candidates_html) + strlen(temp_buffer) < sizeof(candidates_html)) {
            strcat(candidates_html, temp_buffer);
        }
    }
    
    sprintf(body,
        "<div class='container mx-auto p-4 md:p-8 max-w-2xl'>"
        "<div class='bg-white rounded-2xl shadow-xl p-8'>"
        "<h1 class='text-3xl font-bold text-center text-gray-900 mb-8'>Online Voting Portal</h1>"
        "<div class='mb-10'><h2 class='text-2xl font-semibold mb-6 border-b pb-3'>Cast Your Vote</h2>"
        "<form action='/submit_vote' method='POST'><div class='mb-4'><label for='aadhar' class='block text-sm font-medium text-gray-700 mb-1'>Aadhar Number</label><input type='text' id='aadhar' name='aadhar' class='w-full px-4 py-2 bg-gray-50 border rounded-lg' required></div>"
        "<div class='mb-6'><label for='name' class='block text-sm font-medium text-gray-700 mb-1'>Full Name</label><input type='text' id='name' name='name' class='w-full px-4 py-2 bg-gray-50 border rounded-lg' required></div>"
        "<div class='mb-6'><label class='block text-sm font-medium text-gray-700 mb-2'>Select a Candidate</label><div class='space-y-3'>%s</div></div>"
        "<button type='submit' class='w-full bg-blue-600 text-white font-bold py-3 rounded-lg hover:bg-blue-700 transition'>Submit Vote</button></form></div>"
        "<div><h2 class='text-2xl font-semibold mb-6 border-b pb-3'>Admin Panel</h2><form action='/results' method='POST'>"
        "<div class='mb-4'><label for='password' class='block text-sm font-medium text-gray-700 mb-1'>Admin Password</label><input type='password' id='password' name='password' class='w-full px-4 py-2 bg-gray-50 border rounded-lg' required></div>"
        "<button type='submit' class='w-full bg-indigo-600 text-white font-bold py-3 rounded-lg hover:bg-indigo-700 transition'>View Results</button></form></div>"
        "</div></div>",
        candidates_html);

    return generate_html_shell("Online Voting Portal", body);
}

const char *generate_results_page() {
    char body[8192];
    char svg_chart[4096];
    char winner_text[256];
    int max_votes = -1;
    int winner_id = -1;
    int total_votes = 0;
    
    get_vote_counts();
    for(int i = 0; i < num_candidates; i++) {
        total_votes += candidates[i].votes;
        if (candidates[i].votes > max_votes) {
            max_votes = candidates[i].votes;
            winner_id = i;
        }
    }

    int tie = 0;
    if(max_votes > 0) {
        for(int i = 0; i < num_candidates; i++) {
            if (i != winner_id && candidates[i].votes == max_votes) {
                tie = 1;
                break;
            }
        }
    }

    if (tie) {
        strcpy(winner_text, "There is currently a tie.");
    } else if (winner_id != -1) {
        sprintf(winner_text, "Current Winner: <span class='font-bold text-blue-600'>%s</span> with %d votes.", candidates[winner_id].name, max_votes);
    } else {
        strcpy(winner_text, "No votes have been cast yet.");
    }

    generate_results_svg(svg_chart, sizeof(svg_chart));
    
    sprintf(body,
        "<div class='bg-white rounded-2xl shadow-xl p-8 max-w-2xl w-full'>"
        "<h1 class='text-3xl font-bold text-gray-900 mb-2 text-center'>Election Results</h1>"
        "<p class='text-center text-gray-600 mb-6'>Total Votes Cast: %d</p>"
        "<div class='mb-6'>%s</div>"
        "<p class='text-center text-lg mt-4'>%s</p>"
        "<div class='mt-8 text-center'><a href='/' class='text-blue-600 hover:underline'>Back to Portal</a></div></div>",
        total_votes, svg_chart, winner_text);

    return generate_html_shell("Voting Results", body);
}

// --- MHD Handlers ---

static enum MHD_Result iterate_post(void *coninfo_cls, enum MHD_ValueKind kind, const char *key,
                                  const char *filename, const char *content_type,
                                  const char *transfer_encoding, const char *data, uint64_t off, size_t size) {
    struct connection_info_struct *con_info = coninfo_cls;

    if (0 == strcmp(key, "action")) { strncpy(con_info->action, data, 49); }
    if (0 == strcmp(key, "aadhar")) { strncpy(con_info->aadhar, data, 19); }
    if (0 == strcmp(key, "name")) { strncpy(con_info->name, data, 99); }
    if (0 == strcmp(key, "candidate")) { strncpy(con_info->candidate_str, data, 9); }
    if (0 == strcmp(key, "password")) { strncpy(con_info->password, data, 49); }
    
    return MHD_YES;
}

static void request_completed(void *cls, struct MHD_Connection *connection,
                              void **con_cls, enum MHD_RequestTerminationCode toe) {
    struct connection_info_struct *con_info = *con_cls;
    if (NULL == con_info) return;
    if (con_info->postprocessor) {
        MHD_destroy_post_processor(con_info->postprocessor);
    }
    free(con_info);
    *con_cls = NULL;
}

static enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                                     const char *url, const char *method,
                                     const char *version, const char *upload_data,
                                     size_t *upload_data_size, void **con_cls) {
    
    if (NULL == *con_cls) {
        struct connection_info_struct *con_info;
        con_info = malloc(sizeof(struct connection_info_struct));
        if (NULL == con_info) return MHD_NO;
        memset(con_info, 0, sizeof(struct connection_info_struct));
        *con_cls = (void *)con_info;
        return MHD_YES;
    }

    struct connection_info_struct *con_info = *con_cls;
    const char *page = "<html><body>Internal Server Error</body></html>";
    int status_code = 500;
    struct MHD_Response *response;

    if (0 == strcmp(method, "POST")) {
        if (*upload_data_size != 0) {
            if (con_info->postprocessor == NULL) {
                con_info->postprocessor = MHD_create_post_processor(connection, 1024, iterate_post, (void*)con_info);
                if (NULL == con_info->postprocessor) {
                    free(con_info);
                    return MHD_NO;
                }
            }
            MHD_post_process(con_info->postprocessor, upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        } else {
            // POST request finished, now process the data
            if (0 == strcmp(url, "/submit_vote")) {
                if (!is_voter_registered(con_info->aadhar, con_info->name)) {
                    page = generate_message_page("Validation Failed", "Your Aadhar and Name do not match our records.", 0);
                } else if (has_voted(con_info->aadhar)) {
                    page = generate_message_page("Already Voted", "This Aadhar number has already been used to cast a vote.", 0);
                } else {
                    record_vote(atoi(con_info->candidate_str));
                    record_voter_turnout(con_info->aadhar);
                    page = generate_message_page("Success!", "Your vote has been successfully recorded.", 1);
                }
            } else if (0 == strcmp(url, "/results")) {
                if (strcmp(con_info->password, ADMIN_PASS) == 0) {
                    page = generate_results_page();
                } else {
                    page = generate_message_page("Access Denied", "The password you entered is incorrect.", 0);
                }
            }
            status_code = 200;
        }
    } else if (0 == strcmp(method, "GET")) {
        page = generate_voting_page();
        status_code = 200;
    }

    response = MHD_create_response_from_buffer(strlen(page), (void*)page, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "text/html");
    enum MHD_Result ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}


int main() {
    // Create necessary files if they don't exist
    FILE *f;
    if ((f = fopen(CANDIDATES_FILE, "r")) == NULL) { fclose(fopen(CANDIDATES_FILE, "w")); } else { fclose(f); }
    if ((f = fopen(VOTERS_FILE, "r")) == NULL) { fclose(fopen(VOTERS_FILE, "w")); } else { fclose(f); }
    if ((f = fopen(VOTED_FILE, "r")) == NULL) { fclose(fopen(VOTED_FILE, "w")); } else { fclose(f); }
    if ((f = fopen(VOTES_FILE, "r")) == NULL) { fclose(fopen(VOTES_FILE, "w")); } else { fclose(f); }

    load_candidates();
    struct MHD_Daemon *daemon;

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

