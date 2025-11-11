#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <time.h>
#include <math.h> // Added for sin/cos in doughnut chart

// --- Cross-Platform Includes ---
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h> // For file locking & CreateDirectory
    #include <io.h>      // For _get_osfhandle
#else
    // On Linux, these headers are needed for networking and file locking
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <sys/file.h> // For flock()
    #include <unistd.h>   // For fileno()
    #include <fcntl.h>    // For O_RDONLY
    #include <sys/stat.h> // For stat() and mkdir()
#endif

// --- Feature Defines ---
#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif
#define DEFAULT_PORT 8080
#define DEFAULT_ADMIN_PASS "admin123"
#define DEFAULT_ELECTION_NAME "Online Voting Portal" 
#define MAX_UPLOAD_SIZE (5 * 1024 * 1024) // 5 MB

// --- File Paths ---
#define CANDIDATES_FILE "candidates.txt"
#define VOTERS_FILE "voters.txt"
#define VOTED_FILE "voted.txt"
#define VOTES_FILE "votes.txt"
#define ADMIN_PASS_FILE "admin.conf"
#define ELECTION_STATUS_FILE "election_status.conf" 
#define ELECTION_NAME_FILE "election_name.conf" 
#define STATIC_DIR "."       
#define UPLOAD_DIR "images"  
#define TEMP_UPLOAD_FILE "images/upload.tmp" 

// --- Data Structures ---
typedef struct {
    int id;
    char name[100];
    char party[100]; // NEW: Party Name
    char imageUrl[256];
    int votes;
} Candidate;

typedef struct {
    char aadhar[20];
    char name[100];
} Voter;

// Represents the state of a single connection
struct connection_info_struct {
    struct MHD_PostProcessor *postprocessor;
    // Voter form
    char aadhar[20];
    char name[100];
    char candidate_str[10];
    // Admin login/action form
    char password[50];
    // Admin "add candidate" form
    char add_id[10];
    char add_name[100];
    char add_party[100]; // NEW: Party Name
    // Admin "add voter" form
    char add_voter_aadhar[20];
    char add_voter_name[100];
    // Admin "set name" form
    char election_name[100];
    
    // File upload state
    FILE *upload_file_handle;
    char generated_filepath[256];
    char original_filename[256];
    size_t total_upload_size;
    int error_flag; // 1=File Too Large, 2=Bad Type, 3=Write Error, 4=No File, 5=No ID/Name/Party
};

// --- Global Data ---
Candidate *candidates = NULL; 
int num_candidates = 0;
int candidates_array_capacity = 0;
char ADMIN_PASS[100]; 
char ELECTION_STATE[20]; 
char ELECTION_NAME[100]; 

// --- Utility: Cross-Platform File Locking ---
#define LOCK_SHARED 1
#define LOCK_EXCLUSIVE 2

void lock_file(FILE *f, int lock_type) {
    #ifdef _WIN32
        HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(f));
        DWORD dwFlags = (lock_type == LOCK_EXCLUSIVE) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
        OVERLAPPED overlapped = {0};
        LockFileEx(hFile, dwFlags, 0, ~0, ~0, &overlapped);
    #else
        int flock_type = (lock_type == LOCK_EXCLUSIVE) ? LOCK_EX : LOCK_SH;
        flock(fileno(f), flock_type);
    #endif
}

void unlock_file(FILE *f) {
    #ifdef _WIN32
        HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(f));
        OVERLAPPED overlapped = {0};
        UnlockFileEx(hFile, 0, ~0, ~0, &overlapped);
    #else
        flock(fileno(f), LOCK_UN);
    #endif
}


// --- Utility Functions (Data Handling) ---
void load_candidates() {
    if (candidates != NULL) {
        free(candidates);
        candidates = NULL;
    }
    num_candidates = 0;
    candidates_array_capacity = 0;

    FILE *file = fopen(CANDIDATES_FILE, "r");
    if (!file) {
        perror("Could not open candidates file");
        return;
    }
    printf("\n--- Loading Candidates ---\n");
    
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (num_candidates >= candidates_array_capacity) {
            candidates_array_capacity += 10;
            Candidate *new_candidates = realloc(candidates, candidates_array_capacity * sizeof(Candidate));
            if (new_candidates == NULL) {
                perror("Failed to reallocate memory for candidates");
                free(candidates);
                candidates = NULL;
                fclose(file);
                return;
            }
            candidates = new_candidates;
        }

        // MODIFIED: sscanf now parses 4 fields (ID,Name,Party,ImageURL)
        if (sscanf(line, "%d,%99[^,],%99[^,],%255[^\n]", 
                   &candidates[num_candidates].id, 
                   candidates[num_candidates].name, 
                   candidates[num_candidates].party, // NEW
                   candidates[num_candidates].imageUrl) == 4) {
            
            candidates[num_candidates].imageUrl[strcspn(candidates[num_candidates].imageUrl, "\r\n")] = 0;
            printf("Loaded Candidate ID: %d, Name: %s, Party: %s, URL: %s\n", 
                   candidates[num_candidates].id, 
                   candidates[num_candidates].name,
                   candidates[num_candidates].party, // NEW
                   candidates[num_candidates].imageUrl);
            
            candidates[num_candidates].votes = 0;
            num_candidates++;
        }
    }
    printf("--- Finished loading %d candidates ---\n\n", num_candidates);
    fclose(file);
}

int is_voter_registered(const char* aadhar, const char* name) {
    FILE* file = fopen(VOTERS_FILE, "r");
    if (!file) return 0;
    lock_file(file, LOCK_SHARED);

    int found = 0;
    char line[150];
    while (fgets(line, sizeof(line), file)) {
        char file_aadhar[20], file_name[100];
        if (sscanf(line, "%19[^,],%99[^\n]", file_aadhar, file_name) == 2) {
            file_name[strcspn(file_name, "\r\n")] = 0;
            if (strcmp(file_aadhar, aadhar) == 0 && strcmp(file_name, name) == 0) {
                found = 1;
                break;
            }
        }
    }
    
    unlock_file(file);
    fclose(file);
    return found;
}

int has_voted(const char* aadhar) {
    FILE* file = fopen(VOTED_FILE, "r");
    if (!file) return 0;
    lock_file(file, LOCK_SHARED);

    int found = 0;
    char line[20];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, aadhar) == 0) {
            found = 1;
            break;
        }
    }
    
    unlock_file(file);
    fclose(file);
    return found;
}

void record_vote(int candidate_id) {
    FILE* file = fopen(VOTES_FILE, "a");
    if (file) {
        lock_file(file, LOCK_EXCLUSIVE);
        fprintf(file, "%d\n", candidate_id);
        unlock_file(file);
        fclose(file);
    }
}

void record_voter_turnout(const char* aadhar) {
    FILE* file = fopen(VOTED_FILE, "a");
    if (file) {
        lock_file(file, LOCK_EXCLUSIVE);
        fprintf(file, "%s\n", aadhar);
        unlock_file(file);
        fclose(file);
    }
}

void get_vote_counts() {
    for (int i = 0; i < num_candidates; i++) {
        candidates[i].votes = 0;
    }
    FILE* file = fopen(VOTES_FILE, "r");
    if (!file) return;
    lock_file(file, LOCK_SHARED);

    int candidate_id;
    while (fscanf(file, "%d", &candidate_id) == 1) {
        for (int i = 0; i < num_candidates; i++) {
            if (candidates[i].id == candidate_id) {
                candidates[i].votes++;
                break;
            }
        }
    }
    
    unlock_file(file);
    fclose(file);
}

// MODIFIED: Function signature and fprintf now include party
int add_new_candidate(const char* id, const char* name, const char* party, const char* image_url) {
    if (id[0] == '\0' || name[0] == '\0' || party[0] == '\0' || image_url[0] == '\0') {
        return 0;
    }
    
    FILE* file = fopen(CANDIDATES_FILE, "a");
    if (!file) {
        perror("Failed to open candidates file for appending");
        return 0;
    }
    lock_file(file, LOCK_EXCLUSIVE);
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    if (size > 0) {
        fprintf(file, "\n");
    }
    
    int id_num = atoi(id);
    fprintf(file, "%d,%s,%s,%s", id_num, name, party, image_url);
    
    unlock_file(file);
    fclose(file);
    return 1;
}

int add_new_voter(const char* aadhar, const char* name) {
    if (aadhar[0] == '\0' || name[0] == '\0') {
        return 0;
    }

    FILE* file = fopen(VOTERS_FILE, "a");
    if (!file) {
        perror("Failed to open voters file for appending");
        return 0;
    }
    lock_file(file, LOCK_EXCLUSIVE);
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    if (size > 0) {
        fprintf(file, "\n");
    }
    
    fprintf(file, "%s,%s", aadhar, name);
    
    unlock_file(file);
    fclose(file);
    return 1;
}

// --- Election State & Name ---
void load_election_state() {
    FILE* file = fopen(ELECTION_STATUS_FILE, "r");
    if (!file) {
        strcpy(ELECTION_STATE, "PREP");
        FILE* wfile = fopen(ELECTION_STATUS_FILE, "w");
        if (wfile) {
            fprintf(wfile, "PREP\n");
            fclose(wfile);
        }
    } else {
        if (fscanf(file, "%19s", ELECTION_STATE) != 1) {
            strcpy(ELECTION_STATE, "PREP");
        }
        fclose(file);
    }
    printf("--- Election State Loaded: %s ---\n", ELECTION_STATE);
}

void save_election_state(const char* state) {
    FILE* file = fopen(ELECTION_STATUS_FILE, "w");
    if (file) {
        fprintf(file, "%s\n", state);
        fclose(file);
        strcpy(ELECTION_STATE, state);
        printf("--- Election State Saved: %s ---\n", ELECTION_STATE);
    } else {
        perror("CRITICAL: Failed to save election state!");
    }
}

void load_election_name() {
    FILE* file = fopen(ELECTION_NAME_FILE, "r");
    if (!file) {
        strcpy(ELECTION_NAME, DEFAULT_ELECTION_NAME);
        FILE* wfile = fopen(ELECTION_NAME_FILE, "w");
        if (wfile) {
            fprintf(wfile, "%s\n", DEFAULT_ELECTION_NAME);
            fclose(wfile);
        }
    } else {
        if (fgets(ELECTION_NAME, sizeof(ELECTION_NAME), file) == NULL) {
             strcpy(ELECTION_NAME, DEFAULT_ELECTION_NAME);
        }
        ELECTION_NAME[strcspn(ELECTION_NAME, "\r\n")] = 0;
        fclose(file);
    }
    printf("--- Election Name Loaded: %s ---\n", ELECTION_NAME);
}

void save_election_name(const char* name) {
    FILE* file = fopen(ELECTION_NAME_FILE, "w");
    if (file) {
        fprintf(file, "%s\n", name);
        fclose(file);
        strcpy(ELECTION_NAME, name);
        printf("--- Election Name Saved: %s ---\n", ELECTION_NAME);
    } else {
        perror("CRITICAL: Failed to save election name!");
    }
}


int archive_votes_file() {
    FILE* voted_file = fopen(VOTED_FILE, "w");
    if (!voted_file) {
        perror("Failed to clear voted.txt");
        return 0;
    }
    fclose(voted_file);

    FILE* votes_file = fopen(VOTES_FILE, "r");
    if (!votes_file) {
        return 1;
    }
    fseek(votes_file, 0, SEEK_END);
    if (ftell(votes_file) == 0) {
        fclose(votes_file);
        return 1;
    }
    fclose(votes_file); 

    char archive_filename[100];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(archive_filename, sizeof(archive_filename), "votes_archive_%Y%m%d_%H%M%S.txt", t);

    if (rename(VOTES_FILE, archive_filename) != 0) {
        perror("Failed to archive votes.txt");
        return 0;
    }

    FILE* new_votes_file = fopen(VOTES_FILE, "w");
    if (!new_votes_file) {
        perror("Failed to create new votes.txt");
        return 0;
    }
    fclose(new_votes_file);

    return 1;
}

// --- Data Analytics Helper Functions ---
int count_lines_in_file(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) return 0;
    lock_file(file, LOCK_SHARED);
    
    int lines = 0;
    int ch;
    long char_count = 0;
    int last_char = -1;

    while ((ch = fgetc(file)) != EOF) {
        char_count++;
        if (ch == '\n') {
            lines++;
        }
        last_char = ch;
    }

    if (char_count > 0 && last_char != '\n' && lines == 0) {
         lines = 1; 
    } else if (char_count > 0 && last_char != '\n') {
         lines++;
    }

    unlock_file(file);
    fclose(file);
    
    return lines;
}

int get_registered_voter_count() {
    return count_lines_in_file(VOTERS_FILE);
}

int get_cast_vote_count() {
    return count_lines_in_file(VOTED_FILE);
}


// --- HTML/SVG Generation ---
#define PAGE_BUFFER_SIZE 65536 

// MODIFIED: SVG Bar chart now includes party name
void generate_results_svg(char *buffer, size_t buffer_size) {
    get_vote_counts();
    int max_votes = 0;
    for (int i = 0; i < num_candidates; i++) {
        if (candidates[i].votes > max_votes) max_votes = candidates[i].votes;
    }
    if (max_votes == 0) max_votes = 1;

    int chart_width = 500;
    int bar_height = 30;
    int bar_spacing = 15;
    int chart_height = (num_candidates > 0) ? (num_candidates * (bar_height + bar_spacing)) : (bar_height + bar_spacing);

    char svg_buffer[8192] = {0};
    char temp_buffer[1024];

    sprintf(svg_buffer, "<svg width='100%%' viewBox='0 0 %d %d' xmlns='http://www.w3.org/2000/svg' font-family='Inter, sans-serif'>"
                      "<style>"
                      ".bar-rect { transition: width 0.6s ease-out, fill 0.2s ease-in-out; fill: %s; }"
                      ".bar-group:hover .bar-rect { fill: %s; }"
                      "</style>", 
                      chart_width, chart_height, "#3B82F6", "#2563EB");

    for (int i = 0; i < num_candidates; i++) {
        // MODIFIED: bar_width calculation changed to allow more space for text
        int bar_width = (int)(((float)candidates[i].votes / max_votes) * (chart_width - 250)); // Was -150
        if (bar_width < 1) bar_width = 1;
        int y_pos = i * (bar_height + bar_spacing);

        // MODIFIED: Title and text now include party name
        sprintf(temp_buffer, "<g class='bar-group' transform='translate(0 %d)'>"
                             "<title>%s (%s): %d votes</title>"
                             "<rect width='%d' height='%d' rx='6' class='bar-rect'></rect>"
                             "<text x='%d' y='20' fill='#1F2937' font-size='14' font-weight='600'>%s (%s)</text>"
                             "<text x='%d' y='20' fill='#1F2937' font-size='14' font-weight='bold'>%d</text>"
                             "</g>", 
                             y_pos, 
                             candidates[i].name, candidates[i].party, candidates[i].votes,
                             bar_width, bar_height,
                             bar_width + 10, candidates[i].name, candidates[i].party,
                             chart_width - 50, candidates[i].votes);
        strcat(svg_buffer, temp_buffer);
    }
    
    if (num_candidates == 0) {
        sprintf(temp_buffer, "<text x='10' y='20' fill='#6B7280'>No candidates have been added yet.</text>");
        strcat(svg_buffer, temp_buffer);
    }

    strcat(svg_buffer, "</svg>");
    strncpy(buffer, svg_buffer, buffer_size - 1);
}

// (generate_turnout_gauge_svg is unchanged)
void generate_turnout_gauge_svg(char *buffer, size_t buffer_size, int cast_votes, int registered_voters) {
    float turnout_percent = 0.0;
    if (registered_voters > 0) {
        turnout_percent = ((float)cast_votes / (float)registered_voters);
    }
    if (turnout_percent > 1.0) turnout_percent = 1.0; 

    int cx = 100, cy = 100, radius = 80, stroke_width = 20;
    float circumference = M_PI * radius; 
    float offset = circumference * (1.0 - turnout_percent);
    
    char svg_buffer[4096] = {0};
    sprintf(svg_buffer,
        "<svg width='100%%' viewBox='0 0 200 120' xmlns='http://www.w3.org/2000/svg' font-family='Inter, sans-serif'>"
        " <style>"
        "  .gauge-bg { fill: none; stroke: #E5E7EB; stroke-width: %d; }"
        "  .gauge-fg { fill: none; stroke: #10B981; stroke-width: %d; stroke-dasharray: %f; stroke-dashoffset: %f; stroke-linecap: round; transition: stroke-dashoffset 0.8s ease-out; }"
        "  .gauge-text { fill: #1F2937; font-size: 32px; font-weight: 800; text-anchor: middle; }"
        "  .gauge-subtext { fill: #6B7280; font-size: 14px; font-weight: 500; text-anchor: middle; }"
        " </style>"
        " <path class='gauge-bg' d='M %d %d A %d %d 0 0 1 %d %d' />"
        " <path class='gauge-fg' d='M %d %d A %d %d 0 0 1 %d %d' />"
        " <text x='100' y='90' class='gauge-text'>%.0f%%</text>"
        " <text x='100' y='110' class='gauge-subtext'>%d / %d Voters</text>"
        "</svg>",
        stroke_width, stroke_width, circumference, offset,
        cx - radius, cy, radius, radius, cx + radius, cy,
        cx - radius, cy, radius, radius, cx + radius, cy,
        turnout_percent * 100.0,
        cast_votes, registered_voters
    );
    strncpy(buffer, svg_buffer, buffer_size - 1);
}

// MODIFIED: Doughnut chart legend now includes party name
void generate_doughnut_chart_svg(char *buffer, size_t buffer_size, int total_votes) {
    float total_votes_safe = (total_votes == 0) ? 1.0 : (float)total_votes;
    
    const char *colors[] = {"#3B82F6", "#8B5CF6", "#10B981", "#F59E0B", "#EF4444", "#6366F1", "#EC4899", "#14B8A6"};
    int num_colors = sizeof(colors) / sizeof(colors[0]);

    int cx = 100, cy = 100, radius = 80, stroke_width = 20;
    float circumference = 2 * M_PI * radius;
    float current_offset = 0;
    
    char svg_buffer[8192] = {0};
    char temp_buffer[1024];
    
    sprintf(svg_buffer,
        "<div class='flex flex-col md:flex-row items-center justify-between gap-6'>"
        " <div class='relative w-48 h-48'>"
        "  <svg width='100%%' height='100%%' viewBox='0 0 200 200' xmlns='http://www.w3.org/2000/svg' style='transform: rotate(-90deg)'>"
        "   <style>.slice { fill: none; stroke-width: %d; stroke-linecap: butt; transition: stroke-dashoffset 0.6s ease-out; }</style>",
        stroke_width);
    
    for (int i = 0; i < num_candidates; i++) {
        float percent = (float)candidates[i].votes / total_votes_safe;
        float dash_length = circumference * percent;
        float dash_gap = circumference - dash_length;

        sprintf(temp_buffer,
            "<circle class='slice' cx='%d' cy='%d' r='%d' stroke='%s' stroke-dasharray='%f %f' stroke-dashoffset='-%f' />",
            cx, cy, radius, colors[i % num_colors], dash_length, dash_gap, current_offset);
        strcat(svg_buffer, temp_buffer);
        
        current_offset += dash_length;
    }
    
    sprintf(temp_buffer,
        "  </svg>"
        "  <div class='absolute inset-0 flex flex-col items-center justify-center'>"
        "   <span class='text-3xl font-extrabold text-gray-900'>%d</span>"
        "   <span class='text-sm font-medium text-gray-500'>Total Votes</span>"
        "  </div>"
        " </div>"
        " <div class='flex-grow pl-6 space-y-2'>",
        total_votes);
    strcat(svg_buffer, temp_buffer);

    for (int i = 0; i < num_candidates; i++) {
        float percent = (float)candidates[i].votes / total_votes_safe * 100.0;
        // MODIFIED: Legend now includes party name
        sprintf(temp_buffer,
            "<div class='flex items-center justify-between text-sm'>"
            " <div class='flex items-center'>"
            "  <span class='w-3 h-3 rounded-full mr-2' style='background-color: %s;'></span>"
            "  <span class='font-medium text-gray-700'>%s (%s)</span>"
            " </div>"
            " <span class='font-bold text-gray-900'>%.0f%%</span>"
            "</div>",
            colors[i % num_colors], candidates[i].name, candidates[i].party, percent);
        strcat(svg_buffer, temp_buffer);
    }
    
    if (num_candidates == 0) {
        strcat(svg_buffer, "<span class='text-sm text-gray-500'>No votes cast yet.</span>");
    }

    strcat(svg_buffer, "</div></div>");
    strncpy(buffer, svg_buffer, buffer_size - 1);
}

// (generate_voter_list_html is unchanged)
void generate_voter_list_html(char *buffer, size_t buffer_size) {
    FILE* file = fopen(VOTERS_FILE, "r");
    if (!file) {
        strcpy(buffer, "<p class='text-sm text-gray-500'>Could not load voters file.</p>");
        return;
    }
    lock_file(file, LOCK_SHARED);
    
    char list_html[4096] = "<ul class='space-y-2'>";
    char temp_buffer[256];
    char line[150];
    int count = 0;

    while (fgets(line, sizeof(line), file)) {
        char file_aadhar[20], file_name[100];
        if (sscanf(line, "%19[^,],%99[^\n]", file_aadhar, file_name) == 2) {
            file_name[strcspn(file_name, "\r\n")] = 0;
            sprintf(temp_buffer, "<li class='flex justify-between items-center text-sm bg-gray-50 p-2 rounded'>"
                                 " <span class='font-medium text-gray-700'>%s</span>"
                                 " <span class='text-gray-500'>%s</span>"
                                 "</li>", file_name, file_aadhar);
            if (strlen(list_html) + strlen(temp_buffer) < sizeof(list_html) - 6) {
                 strcat(list_html, temp_buffer);
                 count++;
            }
        }
    }
    
    unlock_file(file);
    fclose(file);
    
    if (count == 0) {
        strcpy(buffer, "<p class='text-sm text-gray-500 text-center py-4'>No voters have been registered yet.</p>");
    } else {
        strcat(list_html, "</ul>");
        strncpy(buffer, list_html, buffer_size - 1);
    }
}


// (generate_html_shell is unchanged)
const char* generate_html_shell(const char* title, const char* body, const char* active_page, const char* flash_message) {
    static char page[PAGE_BUFFER_SIZE];
    char nav_home_class[128] = "text-gray-700 font-medium hover:text-blue-600 transition duration-200";
    char nav_admin_class[128] = "text-gray-700 font-medium hover:text-blue-600 transition duration-200";
    char flash_html[512] = "";

    if (active_page && 0 == strcmp(active_page, "Home")) {
        strcpy(nav_home_class, "text-blue-600 font-bold");
    } else if (active_page && 0 == strcmp(active_page, "Admin")) {
        strcpy(nav_admin_class, "text-blue-600 font-bold");
    }

    if (flash_message && flash_message[0] != '\0') {
        const char* flash_bg = (strstr(flash_message, "Success") || strstr(flash_message, "added") || strstr(flash_message, "Started") || strstr(flash_message, "Stopped") || strstr(flash_message, "Reset") || strstr(flash_message, "Set")) 
                               ? "bg-green-100 border-green-500 text-green-700" 
                               : "bg-red-100 border-red-500 text-red-700";
        sprintf(flash_html, 
            "<div class='fade-in fixed top-20 left-1/2 -translate-x-1/2 z-[100] px-6 py-3 rounded-xl border %s shadow-lg'>"
            " <p class='font-semibold'>%s</p>"
            "</div>", flash_bg, flash_message);
    }

    const char* svg_logo = 
        "<a href='/' class='flex items-center space-x-2'>"
        " <svg class='w-8 h-8 text-indigo-700' xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='currentColor'>"
        "  <path d='M11.25 4.5A2.25 2.25 0 109 6.75V15h1.5V6.75A2.25 2.25 0 0011.25 4.5z' />"
        "  <path fill-rule='evenodd' d='M12 2.25c-5.385 0-9.75 4.365-9.75 9.75s4.365 9.75 9.75 9.75 9.75-4.365 9.75-9.75S17.385 2.25 12 2.25zM9 15.75H6v-3a.75.75 0 00-1.5 0v3H3a.75.75 0 000 1.5h1.5v3a.75.75 0 001.5 0v-3h3a.75.75 0 000-1.5zm6-3.75a.75.75 0 100-1.5.75.75 0 000 1.5zM15 15a.75.75 0 100-1.5.75.75 0 000 1.5zM18 12a.75.75 0 100-1.5.75.75 0 000 1.5z' clip-rule='evenodd' />"
        " </svg>"
        " <span class='text-2xl font-bold text-indigo-700'>E-Voting</span>"
        "</a>";
        
    sprintf(page,
        "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>%s</title><script src='https://cdn.tailwindcss.com'></script>"
        "<link href='https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&display=swap' rel='stylesheet'>"
        "<style>"
        " body { font-family: 'Inter', sans-serif; padding-top: 80px; }"
        " @keyframes fadeIn { from { opacity: 0; transform: translateY(20px); } to { opacity: 1; transform: translateY(0); } }"
        " .fade-in { animation: fadeIn 0.8s ease-out forwards; }"
        " .has-[:checked]:ring-2 { box-shadow: 0 0 0 2px #3B82F6; }"
        " .backdrop-blur-xl { backdrop-filter: blur(24px); -webkit-backdrop-filter: blur(24px); }"
        " summary { cursor: pointer; list-style: none; } "
        " summary::-webkit-details-marker { display: none; } "
        " details[open] summary .arrow { transform: rotate(90deg); } "
        " .arrow { transition: transform 0.2s; display: inline-block; }"
        " .aspect-\\[3\\/4\\] { aspect-ratio: 3 / 4; }" // NEW: Custom aspect ratio class
        "</style></head>"
        "<body class='bg-gradient-to-br from-slate-100 to-blue-50 min-h-screen p-4'>"
        
        "<nav class='fixed top-0 left-0 right-0 bg-white/70 backdrop-blur-xl shadow-lg z-50'>"
        " <div class='max-w-6xl mx-auto px-4'>"
        "  <div class='flex justify-between items-center h-16'>"
        "   %s"
        "   <div class='flex space-x-6'>"
        "    <a href='/' class='%s'>Home</a>"
        "    <a href='/admin' class='%s'>Admin</a>"
        "   </div>"
        "  </div>"
        " </div>"
        "</nav>"
        
        "%s" // Flash Message
        "<main class='w-full'>%s</main>" // Page Content WRAPPED in main
        "</body></html>",
        title, svg_logo, nav_home_class, nav_admin_class, flash_html, body);
    return page;
}

const char* generate_message_page(const char* title, const char* message, int is_success) {
    char body[2048];
    const char* success_svg = 
        "<svg class='w-16 h-16 text-green-500 mx-auto' fill='none' stroke='currentColor' viewBox='0 0 24 24' xmlns='http://www.w3.org/2000/svg'>"
        "<path stroke-linecap='round' stroke-linejoin='round' stroke-width='2' d='M9 12l2 2 4-4m6 2a9 9 0 11-18 0 9 9 0 0118 0z'></path></svg>";
    const char* error_svg = 
        "<svg class='w-16 h-16 text-red-500 mx-auto' fill='none' stroke='currentColor' viewBox='0 0 24 24' xmlns='http://www.w3.org/2000/svg'>"
        "<path stroke-linecap='round' stroke-linejoin='round' stroke-width='2' d='M10 14l2-2m0 0l2-2m-2 2l-2-2m2 2l2 2m7-2a9 9 0 11-18 0 9 9 0 0118 0z'></path></svg>";
    
    sprintf(body,
        "<div class='flex items-center justify-center' style='min-height: calc(100vh - 80px);'>"
        "<div class='fade-in bg-white/70 backdrop-blur-xl rounded-2xl shadow-2xl p-8 max-w-lg text-center'>"
        "<div class='mb-4'>%s</div>"
        "<h1 class='text-3xl font-bold %s mb-4'>%s</h1>"
        "<p class='text-gray-700 text-lg'>%s</p>"
        "<div class='mt-8'><a href='/' class='text-blue-600 font-semibold hover:underline transition duration-200'>&larr; Go Back to Portal</a></div></div></div>",
        is_success ? success_svg : error_svg,
        is_success ? "text-gray-900" : "text-gray-900", title, message);
    return generate_html_shell(title, body, "Message", NULL);
}

// MODIFIED: generate_voting_page now has 3:4 aspect ratio and displays party name
const char *generate_voting_page() {
    if (strcmp(ELECTION_STATE, "LIVE") != 0) {
        const char* title = (strcmp(ELECTION_STATE, "PREP") == 0) ? "Voting Has Not Started" : "Voting Has Closed";
        const char* message = (strcmp(ELECTION_STATE, "PREP") == 0) 
            ? "The election is not yet open for voting. Please check back later." 
            : "The voting period has ended. Results will be announced soon.";
        
        char body[2048];
        const char* info_svg = 
            "<svg class='w-16 h-16 text-blue-500 mx-auto' fill='none' stroke='currentColor' viewBox='0 0 24 24' xmlns='http://www.w3.org/2000/svg'>"
            "<path stroke-linecap='round' stroke-linejoin='round' stroke-width='2' d='M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z'></path></svg>";
        
        sprintf(body,
            "<div class='flex items-center justify-center' style='min-height: calc(100vh - 80px);'>"
            "<div class='fade-in bg-white/70 backdrop-blur-xl rounded-2xl shadow-2xl p-8 max-w-lg text-center'>"
            "<div class='mb-4'>%s</div>"
            "<h1 class='text-3xl font-bold text-gray-900 mb-4'>%s</h1>"
            "<p class='text-gray-700 text-lg'>%s</p>"
            "</div></div>", info_svg, title, message);
        return generate_html_shell(title, body, "Home", NULL);
    }

    char body[16384];
    char candidates_html[8192] = "";
    char temp_buffer[2048];

    for (int i = 0; i < num_candidates; i++) {
        snprintf(temp_buffer, sizeof(temp_buffer),
            "<label for='cand%d' class='flex flex-col bg-white/80 rounded-xl border border-gray-200 shadow-sm cursor-pointer transition duration-300 ease-in-out hover:shadow-lg hover:border-blue-400 hover:-translate-y-1 has-[:checked]:ring-2 has-[:checked]:ring-blue-500 has-[:checked]:border-blue-500 overflow-hidden'>" 
            
            // MODIFIED: aspect-video changed to aspect-[3/4]
            "<img src='%s' alt='%s' class='w-full aspect-[3/4] object-cover' onerror=\"this.src='https://placehold.co/600x800/E0E7FF/3730A3?text=3:4+IMG'; this.onerror=null;\">"
            
            // MODIFIED: Added Party Name
            "<div class='flex items-center justify-between p-4'>"
            " <div>"
            "  <span class='text-lg font-semibold text-gray-900'>%s</span>"
            "  <p class='text-sm text-gray-500'>%s</p>" // NEW: Party name
            " </div>"
            "  <input id='cand%d' name='candidate' type='radio' value='%d' class='h-5 w-5 text-blue-600 border-gray-300 focus:ring-blue-500' required>"
            "</div>"
            "</label>",
            candidates[i].id, 
            candidates[i].imageUrl, candidates[i].name,
            candidates[i].name,
            candidates[i].party, // NEW
            candidates[i].id, candidates[i].id
        );
        
        if (strlen(candidates_html) + strlen(temp_buffer) < sizeof(candidates_html)) {
            strcat(candidates_html, temp_buffer);
        }
    }
    
    sprintf(body,
        "<div class='container mx-auto p-4 md:p-8 max-w-3xl'>"
        "<div class='fade-in bg-white/70 backdrop-blur-xl rounded-3xl shadow-2xl p-8 md:p-12'>"
        "<h1 class='text-4xl font-extrabold text-center text-gray-900 mb-10'>%s</h1>" 
        
        "<div class='mb-10'><h2 class='text-2xl font-semibold mb-6 border-b border-gray-300 pb-3 text-gray-800'>Cast Your Vote</h2>"
        "<form action='/submit_vote' method='POST' class='space-y-6'>"
        "<div><label for='aadhar' class='block text-sm font-medium text-gray-700 mb-1'>Aadhar Number</label>"
        "<input type='text' id='aadhar' name='aadhar' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500 focus:border-transparent' required></div>"
        "<div><label for='name' class='block text-sm font-medium text-gray-700 mb-1'>Full Name</label>"
        "<input type='text' id='name' name='name' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500 focus:border-transparent' required></div>"
        
        "<div><label class='block text-sm font-medium text-gray-700 mb-2'>Select a Candidate</label><div class='grid grid-cols-1 sm:grid-cols-2 gap-4'>%s</div></div>"
        "<button type='submit' class='w-full bg-blue-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg transform transition duration-200 hover:scale-105 hover:bg-blue-700 hover:shadow-xl focus:outline-none focus:ring-2 focus:ring-blue-500 focus:ring-offset-2'>Submit Vote</button></form></div>"
        
        "</div></div>",
        ELECTION_NAME, candidates_html);

    return generate_html_shell("Online Voting Portal", body, "Home", NULL);
}

const char *generate_admin_login_page() {
    char body[4096];
    sprintf(body,
        "<div class='flex items-center justify-center' style='min-height: calc(100vh - 80px);'>"
        "<div class='fade-in bg-white/70 backdrop-blur-xl rounded-3xl shadow-2xl p-8 md:p-12 max-w-md w-full'>"
        "<h1 class='text-4xl font-extrabold text-center text-gray-900 mb-10'>Admin Login</h1>"
        "<form action='/results' method='POST' class='space-y-6'>"
        "<div><label for='password' class='block text-sm font-medium text-gray-700 mb-1'>Admin Password</label>"
        "<input type='password' id='password' name='password' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:border-transparent' required></div>"
        "<button type='submit' class='w-full bg-indigo-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg transform transition duration-200 hover:scale-105 hover:bg-indigo-700 hover:shadow-xl focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2'>Login</button></form>"
        "</div></div>"
    );
    return generate_html_shell("Admin Login", body, "Admin", NULL);
}

// MODIFIED: Admin dashboard now has new "Add Party" field
const char *generate_admin_dashboard_page(const char* password, const char* flash_message) {
    char body[32768]; 
    char svg_bar_chart[8192];
    char svg_gauge_chart[4096];
    char svg_doughnut_chart[8192];
    char voter_list_html[4096];
    char winner_text[256];
    
    get_vote_counts();
    int registered_voters = get_registered_voter_count();
    int cast_votes = get_cast_vote_count();
    
    int total_votes = 0;
    int max_votes = -1;
    int winner_id = -1;
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
    } else if (winner_id != -1 && max_votes > 0) {
        sprintf(winner_text, "Current Winner: <span class='font-bold text-blue-700'>%s (%s)</span> with %d votes", candidates[winner_id].name, candidates[winner_id].party, max_votes);
    } else {
        strcpy(winner_text, "No votes have been cast yet.");
    }

    generate_turnout_gauge_svg(svg_gauge_chart, sizeof(svg_gauge_chart), cast_votes, registered_voters);
    generate_doughnut_chart_svg(svg_doughnut_chart, sizeof(svg_doughnut_chart), total_votes);
    generate_results_svg(svg_bar_chart, sizeof(svg_bar_chart));
    generate_voter_list_html(voter_list_html, sizeof(voter_list_html));
    
    // MODIFIED: "Add Candidate" form now has "Party Name" field
    const char* add_candidate_form = 
        "<details class='bg-white/50 rounded-xl shadow-inner'>"
        " <summary class='p-5 font-semibold text-lg text-gray-800 flex justify-between items-center cursor-pointer'>"
        "  Add New Candidate"
        "  <span class='arrow text-indigo-600'>&#9654;</span>"
        " </summary>"
        " <div class='p-6 border-t border-gray-200'>"
        "  <form action='/add_candidate' method='POST' enctype='multipart/form-data' class='space-y-6'>"
        "   <div><label for='add_id' class='block text-sm font-medium text-gray-700 mb-1'>Candidate ID (must be a number)</label>"
        "   <input type='text' id='add_id' name='add_id' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500' required></div>"
        "   <div><label for='add_name' class='block text-sm font-medium text-gray-700 mb-1'>Candidate Name</label>"
        "   <input type='text' id='add_name' name='add_name' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500' required></div>"
        "   <div><label for='add_party' class='block text-sm font-medium text-gray-700 mb-1'>Party Name</label>" // NEW
        "   <input type='text' id='add_party' name='add_party' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500' required></div>" // NEW
        "   <div><label for='add_image_file' class='block text-sm font-medium text-gray-700 mb-1'>Candidate Image (PNG or JPG)</label>"
        "   <input type='file' id='add_image_file' name='add_image_file' accept='image/png, image/jpeg' class='block w-full text-sm text-gray-700 file:mr-4 file:py-2 file:px-4 file:rounded-lg file:border-0 file:text-sm file:font-semibold file:bg-indigo-50 file:text-indigo-700 hover:file:bg-indigo-100' required></div>"
        "   <p class='text-xs text-gray-500'>Max file size: 5MB.</p>"
        "   <input type='hidden' name='password' value='%s'>"
        "   <button type='submit' class='w-full bg-green-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg transform transition duration-200 hover:scale-105 hover:bg-green-700 hover:shadow-xl focus:outline-none focus:ring-2 focus:ring-green-500'>Add Candidate</button>"
        "  </form>"
        " </div>"
        "</details>";
    char add_candidate_form_with_pass[4096];
    sprintf(add_candidate_form_with_pass, add_candidate_form, password); 

    const char* add_voter_form_template = 
        "<details class='bg-white/50 rounded-xl shadow-inner'>"
        " <summary class='p-5 font-semibold text-lg text-gray-800 flex justify-between items-center cursor-pointer'>"
        "  Add New Voter"
        "  <span class='arrow text-indigo-600'>&#9654;</span>"
        " </summary>"
        " <div class='p-6 border-t border-gray-200'>"
        "  <form action='/add_voter' method='POST' class='space-y-6'>"
        "   <div><label for='add_voter_aadhar' class='block text-sm font-medium text-gray-700 mb-1'>Voter Aadhar</label>"
        "   <input type='text' id='add_voter_aadhar' name='add_voter_aadhar' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500' required></div>"
        "   <div><label for='add_voter_name' class='block text-sm font-medium text-gray-700 mb-1'>Voter Name</label>"
        "   <input type='text' id='add_voter_name' name='add_voter_name' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500' required></div>"
        "   <input type='hidden' name='password' value='%s'>"
        "   <button type='submit' class='w-full bg-green-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg transform transition duration-200 hover:scale-105 hover:bg-green-700 hover:shadow-xl focus:outline-none focus:ring-2 focus:ring-green-500'>Add Voter</button>"
        "  </form>"
        " </div>"
        "</details>";
    char add_voter_form[4096];
    sprintf(add_voter_form, add_voter_form_template, password);

    char election_control_html[2048];
    char status_color[50];
    if (strcmp(ELECTION_STATE, "LIVE") == 0) {
        strcpy(status_color, "text-green-600"); 
    } else if (strcmp(ELECTION_STATE, "CLOSED") == 0) {
        strcpy(status_color, "text-red-600");
    } else {
        strcpy(status_color, "text-yellow-600"); 
    }
    sprintf(election_control_html, 
        "<div class='bg-white/50 p-6 rounded-xl shadow-inner'>"
        " <p class='text-center text-lg mb-4'>Current Status: <span class='font-bold %s'>%s</span></p>"
        " <div class='grid grid-cols-3 gap-4'>"
        "  <form action='/start_election' method='POST'>"
        "   <input type='hidden' name='password' value='%s'>"
        "   <button type='submit' class='w-full bg-green-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg transform transition hover:scale-105 hover:bg-green-700' %s>START</button>"
        "  </form>"
        "  <form action='/stop_election' method='POST'>"
        "   <input type='hidden' name='password' value='%s'>"
        "   <button type='submit' class='w-full bg-red-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg transform transition hover:scale-105 hover:bg-red-700' %s>STOP</button>"
        "  </form>"
        "  <form action='/reset_election' method='POST' onsubmit=\"return confirm('Are you sure you want to reset the election? This will archive all votes and clear the voter turnout list.');\">"
        "   <input type='hidden' name='password' value='%s'>"
        "   <button type='submit' class='w-full bg-gray-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg transform transition hover:scale-105 hover:bg-gray-700' %s>RESET</button>"
        "  </form>"
        " </div>"
        "</div>",
        status_color, ELECTION_STATE, 
        password, (strcmp(ELECTION_STATE, "LIVE") == 0) ? "disabled class='opacity-50 cursor-not-allowed w-full bg-green-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg'" : "",
        password, (strcmp(ELECTION_STATE, "LIVE") != 0) ? "disabled class='opacity-50 cursor-not-allowed w-full bg-red-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg'" : "",
        password, (strcmp(ELECTION_STATE, "LIVE") == 0) ? "disabled class='opacity-50 cursor-not-allowed w-full bg-gray-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg'" : ""
    );

    char election_settings_form[2048];
    sprintf(election_settings_form,
        "<div class='bg-white/50 p-6 rounded-xl shadow-inner'>"
        " <form action='/set_election_name' method='POST' class='space-y-4'>"
        "  <div><label for='election_name' class='block text-sm font-medium text-gray-700 mb-1'>Election Name</label>"
        "  <input type='text' id='election_name' name='election_name' value='%s' class='block w-full px-4 py-3 bg-white/80 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500' required></div>"
        "  <input type='hidden' name='password' value='%s'>"
        "  <button type='submit' class='w-full bg-blue-600 text-white font-bold py-3 px-4 rounded-xl shadow-lg transform transition duration-200 hover:scale-105 hover:bg-blue-700'>Set Name</button>"
        " </form>"
        "</div>",
        ELECTION_NAME, password
    );

    sprintf(body,
        "<div class='container mx-auto p-4 md:p-8 max-w-6xl'>"
        "<div class='fade-in bg-white/70 backdrop-blur-xl rounded-3xl shadow-2xl p-8 md:p-12 w-full space-y-12'>"
        "<h1 class='text-4xl font-extrabold text-gray-900 mb-0 text-center'>Admin Dashboard</h1>"
        
        "<section>"
        " <h2 class='text-2xl font-semibold mb-6 border-b border-gray-300 pb-3 text-gray-800'>Data Analytics</h2>"
        " <div class='grid grid-cols-1 md:grid-cols-2 gap-8'>"
        "  <div class='bg-white/50 p-6 rounded-xl shadow-inner'>"
        "   <h3 class='text-lg font-semibold text-gray-800 mb-4 text-center'>Voter Turnout</h3>"
        "   %s"
        "  </div>"
        "  <div class='bg-white/50 p-6 rounded-xl shadow-inner'>"
        "   <h3 class='text-lg font-semibold text-gray-800 mb-4 text-center'>Vote Distribution</h3>"
        "   %s"
        "  </div>"
        " </div>"
        "</section>"

        "<section>"
        " <h2 class='text-2xl font-semibold mb-6 border-b border-gray-300 pb-3 text-gray-800'>Election Control</h2>"
        " <div class='grid grid-cols-1 md:grid-cols-2 gap-8'>"
        "  %s" 
        "  %s"
        " </div>"
        "</section>"

        "<section>"
        " <h2 class='text-2xl font-semibold mb-6 border-b border-gray-300 pb-3 text-gray-800'>Live Results</h2>"
        " <p class='text-center text-lg text-gray-600 mb-8'>Total Votes Cast: <span class='font-bold text-gray-900'>%d</span></p>"
        " <div class='bg-white/50 p-6 rounded-xl shadow-inner mb-6'>%s</div>"
        " <p class='text-center text-xl text-gray-800 mt-6'>%s</p>"
        "</section>"

        "<section>"
        " <h2 class='text-2xl font-semibold mb-6 border-b border-gray-300 pb-3 text-gray-800'>Manage Election Data</h2>"
        " <div class='grid grid-cols-1 md:grid-cols-2 gap-8'>"
        "  <div>"
        "   <h3 class='text-lg font-semibold text-gray-800 mb-4'>Candidates</h3>"
        "   %s"
        "  </div>"
        "  <div>"
        "   <h3 class='text-lg font-semibold text-gray-800 mb-4'>Voters</h3>"
        "   %s"
        "   <div class='mt-4 bg-white/50 p-4 rounded-xl shadow-inner max-h-64 overflow-y-auto'>"
        "    <h4 class='font-semibold text-gray-700 mb-3'>Registered Voter List</h4>"
        "    %s"
        "   </div>"
        "  </div>"
        " </div>"
        "</section>"

        "</div></div>",
        svg_gauge_chart,
        svg_doughnut_chart,
        election_control_html,
        election_settings_form,
        total_votes, 
        svg_bar_chart, 
        winner_text, 
        add_candidate_form_with_pass,
        add_voter_form,
        voter_list_html
    );

    return generate_html_shell("Admin Dashboard", body, "Admin", flash_message);
}


// --- MHD Handlers ---
const char *get_mime_type(const char *filename) {
    if (strstr(filename, ".css")) return "text/css";
    if (strstr(filename, ".js")) return "application/javascript";
    if (strstr(filename, ".jpg")) return "image/jpeg";
    if (strstr(filename, ".jpeg")) return "image/jpeg";
    if (strstr(filename, ".png")) return "image/png";
    if (strstr(filename, ".gif")) return "image/gif";
    if (strstr(filename, ".svg")) return "image/svg+xml";
    if (strstr(filename, ".ico")) return "image/x-icon";
    return "application/octet-stream";
} 

static enum MHD_Result serve_static_file(struct MHD_Connection *connection, const char *url) {
    char filepath[1024];
    if (strstr(url, "..")) {
        return MHD_NO;
    }
    snprintf(filepath, sizeof(filepath), "%s%s", STATIC_DIR, url);

    struct stat st;
    if (stat(filepath, &st) != 0) {
        return MHD_NO;
    }

    #ifdef _WIN32
        int fd = _open(filepath, _O_RDONLY | _O_BINARY);
    #else
        int fd = open(filepath, O_RDONLY);
    #endif
    
    if (fd == -1) {
        return MHD_NO;
    }

    struct MHD_Response *response = MHD_create_response_from_fd(st.st_size, fd);
    if (NULL == response) {
        #ifdef _WIN32
            _close(fd);
        #else
            close(fd);
        #endif
        return MHD_NO;
    }

    const char *mime_type = get_mime_type(url);
    MHD_add_response_header(response, "Content-Type", mime_type);
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

// MODIFIED: iterate_post now handles add_party
static enum MHD_Result iterate_post(void *coninfo_cls, enum MHD_ValueKind kind, const char *key,
                                  const char *filename, const char *content_type,
                                  const char *transfer_encoding, const char *data, uint64_t off, size_t size) {
    
    struct connection_info_struct *con_info = coninfo_cls;
    if (key == NULL) return MHD_YES;

    if (filename == NULL) {
        if (size > 0) {
            if (0 == strcmp(key, "aadhar")) { strncat(con_info->aadhar, data, 19 - strlen(con_info->aadhar)); }
            if (0 == strcmp(key, "name")) { strncat(con_info->name, data, 99 - strlen(con_info->name)); }
            if (0 == strcmp(key, "candidate")) { strncat(con_info->candidate_str, data, 9 - strlen(con_info->candidate_str)); }
            if (0 == strcmp(key, "password")) { strncat(con_info->password, data, 49 - strlen(con_info->password)); }
            if (0 == strcmp(key, "add_id")) { strncat(con_info->add_id, data, 9 - strlen(con_info->add_id)); }
            if (0 == strcmp(key, "add_name")) { strncat(con_info->add_name, data, 99 - strlen(con_info->add_name)); }
            if (0 == strcmp(key, "add_party")) { strncat(con_info->add_party, data, 99 - strlen(con_info->add_party)); } // NEW
            if (0 == strcmp(key, "add_voter_aadhar")) { strncat(con_info->add_voter_aadhar, data, 19 - strlen(con_info->add_voter_aadhar)); }
            if (0 == strcmp(key, "add_voter_name")) { strncat(con_info->add_voter_name, data, 99 - strlen(con_info->add_voter_name)); }
            if (0 == strcmp(key, "election_name")) { strncat(con_info->election_name, data, 99 - strlen(con_info->election_name)); }
        }
        return MHD_YES;
    }

    if (0 == strcmp(key, "add_image_file")) {
        if (off == 0) {
            if (con_info->upload_file_handle != NULL) {
                fclose(con_info->upload_file_handle);
                con_info->upload_file_handle = NULL;
            }
            if (!content_type || (strcmp(content_type, "image/jpeg") != 0 && strcmp(content_type, "image/png") != 0)) {
                con_info->error_flag = 2;
                return MHD_NO;
            }
            
            strncpy(con_info->original_filename, filename, 255);
            con_info->upload_file_handle = fopen(TEMP_UPLOAD_FILE, "wb");
            if (con_info->upload_file_handle == NULL) {
                con_info->error_flag = 3;
                return MHD_NO;
            }
            con_info->total_upload_size = 0;
            con_info->error_flag = 4;
        }

        if (size > 0) {
            con_info->total_upload_size += size;
            if (con_info->total_upload_size > MAX_UPLOAD_SIZE) {
                con_info->error_flag = 1;
                fclose(con_info->upload_file_handle);
                con_info->upload_file_handle = NULL;
                remove(TEMP_UPLOAD_FILE);
                return MHD_NO;
            }
            
            if (fwrite(data, 1, size, con_info->upload_file_handle) != size) {
                con_info->error_flag = 3;
                fclose(con_info->upload_file_handle);
                con_info->upload_file_handle = NULL;
                remove(TEMP_UPLOAD_FILE);
                return MHD_NO;
            }
            con_info->error_flag = 0;
        }

        if (size == 0) {
            if (con_info->upload_file_handle != NULL) {
                fclose(con_info->upload_file_handle);
                con_info->upload_file_handle = NULL;
            }
        }
    }

    return MHD_YES;
}

static void request_completed(void *cls, struct MHD_Connection *connection,
                              void **con_cls, enum MHD_RequestTerminationCode toe) {
    struct connection_info_struct *con_info = *con_cls;
    if (NULL == con_info) return;
    if (con_info->postprocessor) {
        MHD_destroy_post_processor(con_info->postprocessor);
    }
    if (con_info->upload_file_handle != NULL) {
        fclose(con_info->upload_file_handle);
        remove(TEMP_UPLOAD_FILE);
    }
    free(con_info);
    *con_cls = NULL;
}

// MODIFIED: request_handler now handles add_party
static enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                                     const char *url, const char *method,
                                     const char *version, const char *upload_data,
                                     size_t *upload_data_size, void **con_cls) {
    
    if (NULL == *con_cls) {
        struct connection_info_struct *con_info;
        con_info = calloc(1, sizeof(struct connection_info_struct));
        if (NULL == con_info) return MHD_NO;
        *con_cls = (void *)con_info;
        return MHD_YES;
    }

    struct connection_info_struct *con_info = *con_cls;
    const char *page = "<html><body>Internal Server Error</body></html>";
    int status_code = 500;
    struct MHD_Response *response;
    const char *flash_message = NULL; 

    if (0 == strcmp(method, "POST")) {
        if (*upload_data_size != 0) {
            if (con_info->postprocessor == NULL) {
                con_info->postprocessor = MHD_create_post_processor(connection, 8192, iterate_post, (void*)con_info);
                if (NULL == con_info->postprocessor) {
                    free(con_info);
                    return MHD_NO;
                }
            }
            if (MHD_post_process(con_info->postprocessor, upload_data, *upload_data_size) != MHD_YES) {
                // Error already set by iterate_post
            }
            *upload_data_size = 0;
            return MHD_YES;
        } 
        else {
            if (con_info->postprocessor) {
                MHD_destroy_post_processor(con_info->postprocessor);
                con_info->postprocessor = NULL;
            }
            
            con_info->aadhar[strcspn(con_info->aadhar, "\r\n")] = 0;
            con_info->name[strcspn(con_info->name, "\r\n")] = 0;
            con_info->add_id[strcspn(con_info->add_id, "\r\n")] = 0;
            con_info->add_name[strcspn(con_info->add_name, "\r\n")] = 0;
            con_info->add_party[strcspn(con_info->add_party, "\r\n")] = 0; // NEW
            con_info->add_voter_aadhar[strcspn(con_info->add_voter_aadhar, "\r\n")] = 0;
            con_info->add_voter_name[strcspn(con_info->add_voter_name, "\r\n")] = 0;
            con_info->password[strcspn(con_info->password, "\r\n")] = 0;
            con_info->election_name[strcspn(con_info->election_name, "\r\n")] = 0;

            
            if (0 == strcmp(url, "/submit_vote")) {
                if (strcmp(ELECTION_STATE, "LIVE") != 0) {
                    page = generate_message_page("Voting Not Active", "Voting is not currently open.", 0);
                }
                else if (!is_voter_registered(con_info->aadhar, con_info->name)) {
                    page = generate_message_page("Validation Failed", "Your Aadhar and Name do not match our records.", 0);
                } else if (has_voted(con_info->aadhar)) {
                    page = generate_message_page("Already Voted", "This Aadhar number has already been used to cast a vote.", 0);
                } else if (con_info->candidate_str[0] == '\0') {
                    page = generate_message_page("No Selection", "You did not select a candidate.", 0);
                } else {
                    record_vote(atoi(con_info->candidate_str));
                    record_voter_turnout(con_info->aadhar);
                    page = generate_message_page("Success!", "Your vote has been successfully recorded.", 1);
                }
            } else if (0 == strcmp(url, "/results")) {
                if (strcmp(con_info->password, ADMIN_PASS) == 0) {
                    page = generate_admin_dashboard_page(con_info->password, NULL); 
                } else {
                    page = generate_message_page("Access Denied", "The password you entered is incorrect.", 0);
                }
            } 
            else if (0 == strcmp(url, "/add_candidate")) {
                if (strcmp(con_info->password, ADMIN_PASS) == 0) {
                    // MODIFIED: Check for party name
                    if (con_info->add_id[0] == '\0' || con_info->add_name[0] == '\0' || con_info->add_party[0] == '\0') {
                         flash_message = "Error: Candidate ID, Name, and Party are required.";
                         con_info->error_flag = 5;
                         remove(TEMP_UPLOAD_FILE);
                    } else if (con_info->error_flag == 1) {
                        flash_message = "Error: File is larger than 5MB.";
                    } else if (con_info->error_flag == 2) {
                        flash_message = "Error: Only .jpg or .png images are allowed.";
                    } else if (con_info->error_flag == 3) {
                        flash_message = "Error: Server failed to write file.";
                    } else if (con_info->error_flag == 4 || con_info->original_filename[0] == '\0') {
                        flash_message = "Error: No file was uploaded.";
                    } else {
                        const char *ext = strrchr(con_info->original_filename, '.');
                        if (!ext || (strcmp(ext, ".jpg") != 0 && strcmp(ext, ".png") != 0 && strcmp(ext, ".jpeg") != 0)) {
                            ext = ".jpg";
                        }
                        
                        char final_filepath[256];
                        char url_path[256];

                        snprintf(final_filepath, sizeof(final_filepath), "%s/%s%s", UPLOAD_DIR, con_info->add_id, ext);
                        snprintf(url_path, sizeof(url_path), "/%s/%s%s", UPLOAD_DIR, con_info->add_id, ext);
                        
                        if (rename(TEMP_UPLOAD_FILE, final_filepath) == 0) {
                            // MODIFIED: Pass party name to function
                            if (add_new_candidate(con_info->add_id, con_info->add_name, con_info->add_party, url_path)) {
                                load_candidates(); 
                                flash_message = "Success! Candidate added successfully.";
                            } else {
                                flash_message = "Error: Failed to save candidate to file.";
                            }
                        } else {
                            flash_message = "Error: Failed to save file after upload.";
                            perror("Rename failed");
                            remove(TEMP_UPLOAD_FILE);
                        }
                    }
                } else {
                    flash_message = "Error: Invalid password.";
                    remove(TEMP_UPLOAD_FILE);
                }
                page = generate_admin_dashboard_page(con_info->password, flash_message); 
            }
            else if (0 == strcmp(url, "/add_voter")) {
                if (strcmp(con_info->password, ADMIN_PASS) == 0) {
                    if (con_info->add_voter_aadhar[0] == '\0' || con_info->add_voter_name[0] == '\0') {
                        flash_message = "Error: Voter Aadhar and Name are required.";
                    } else {
                        if (add_new_voter(con_info->add_voter_aadhar, con_info->add_voter_name)) {
                            flash_message = "Success! Voter added successfully.";
                        } else {
                            flash_message = "Error: Failed to save voter to file.";
                        }
                    }
                } else {
                    flash_message = "Error: Invalid password.";
                }
                page = generate_admin_dashboard_page(con_info->password, flash_message);
            }
            else if (0 == strcmp(url, "/start_election")) {
                if (strcmp(con_info->password, ADMIN_PASS) == 0) {
                    save_election_state("LIVE");
                    flash_message = "Success! Election is now LIVE.";
                } else {
                    flash_message = "Error: Invalid password.";
                }
                page = generate_admin_dashboard_page(con_info->password, flash_message);
            }
            else if (0 == strcmp(url, "/stop_election")) {
                if (strcmp(con_info->password, ADMIN_PASS) == 0) {
                    save_election_state("CLOSED");
                    flash_message = "Success! Election is now CLOSED.";
                } else {
                    flash_message = "Error: Invalid password.";
                }
                page = generate_admin_dashboard_page(con_info->password, flash_message);
            }
            else if (0 == strcmp(url, "/reset_election")) {
                if (strcmp(con_info->password, ADMIN_PASS) == 0) {
                    if (archive_votes_file()) {
                        save_election_state("PREP");
                        load_candidates();
                        flash_message = "Success! Election has been reset.";
                    } else {
                        flash_message = "Error: Failed to archive and reset files.";
                    }
                } else {
                    flash_message = "Error: Invalid password.";
                }
                page = generate_admin_dashboard_page(con_info->password, flash_message);
            }
            else if (0 == strcmp(url, "/set_election_name")) {
                 if (strcmp(con_info->password, ADMIN_PASS) == 0) {
                    if (con_info->election_name[0] == '\0') {
                        flash_message = "Error: Election name cannot be empty.";
                    } else {
                        save_election_name(con_info->election_name);
                        flash_message = "Success! Election name has been set.";
                    }
                 } else {
                    flash_message = "Error: Invalid password.";
                 }
                 page = generate_admin_dashboard_page(con_info->password, flash_message);
            }

            status_code = 200;
        }
    } else if (0 == strcmp(method, "GET")) {
        if (0 == strcmp(url, "/")) {
            load_candidates();
            page = generate_voting_page(); 
            status_code = 200;
        } else if (0 == strcmp(url, "/admin")) {
            page = generate_admin_login_page();
            status_code = 200;
        } 
        else if (strncmp(url, "/images/", 8) == 0) {
            if (serve_static_file(connection, url) == MHD_YES) {
                return MHD_YES;
            } else {
                page = generate_message_page("Not Found", "The requested image does not exist.", 0);
                status_code = 404;
            }
        }
        else {
            page = generate_message_page("Not Found", "The page you are looking for does not exist.", 0);
            status_code = 404;
        }
    }

    response = MHD_create_response_from_buffer(strlen(page), (void*)page, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "text/html");
    enum MHD_Result ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}


int main(int argc, char *argv[]) {
    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            fprintf(stderr, "WSAStartup failed.\n");
            return 1;
        }
    #endif

    #ifdef _WIN32
        CreateDirectory(UPLOAD_DIR, NULL);
    #else
        mkdir(UPLOAD_DIR, 0755);
    #endif

    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number '%s'. Using default %d.\n", argv[1], DEFAULT_PORT);
            port = DEFAULT_PORT;
        }
    }

    FILE *pfile = fopen(ADMIN_PASS_FILE, "r");
    if (pfile) {
        if (fscanf(pfile, "%99s", ADMIN_PASS) != 1) {
             strcpy(ADMIN_PASS, DEFAULT_ADMIN_PASS);
        }
        fclose(pfile);
    } else {
        printf("admin.conf not found. Creating with default password.\n");
        pfile = fopen(ADMIN_PASS_FILE, "w");
        if (pfile) {
            fprintf(pfile, "%s\n", DEFAULT_ADMIN_PASS);
            fclose(pfile);
        }
        strcpy(ADMIN_PASS, DEFAULT_ADMIN_PASS);
    }
    printf("Admin password is: %s\n", ADMIN_PASS);

    FILE *f;
    const char* files[] = {CANDIDATES_FILE, VOTERS_FILE, VOTED_FILE, VOTES_FILE, ELECTION_NAME_FILE};
    for(int i = 0; i < 5; i++){
        if ((f = fopen(files[i], "r")) == NULL) {
            fclose(fopen(files[i], "w"));
        } else {
            fclose(f);
        }
    }
    
    load_election_state();
    load_election_name(); 
    load_candidates();
    struct MHD_Daemon *daemon;

    daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, port, NULL, NULL,
                              &request_handler, NULL,
                              MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
                              MHD_OPTION_END);
    if (NULL == daemon) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }

    printf("Server is running on http://localhost:%d\n", port);
    printf("Press Enter to quit...\n");
    getchar();

    MHD_stop_daemon(daemon);

    if (candidates != NULL) {
        free(candidates);
    }

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}