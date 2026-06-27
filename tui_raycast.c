#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/ioctl.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "maps.h"

const float speed = 3.5f;
const float look_speed = 4.0f;
const float smoothing_factor = 0.18f;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define deg_to_rad(deg) ((deg) * M_PI / 180.0f)

#define MAX_THREADS 32

#define PREFIX_STR "\033[H" 
#define PREFIX_LEN 3

const char *charset[] = {
    "# ",
    "#-. ",
    "#*+=-:. ",
    "S&MW$B@%#*+=-:. ",
    "$#|)(1}{][\\?-_+~<>i!lI;:,\"^`'=. ",
    "$@B%8&WMmwbdpqkhOQ0ULCJYXanxuvcrjf/\\|()1{}[]?-_+~<>i!lI;:,\"^`'. "
};

unsigned char ch;

unsigned char render_length = 224;
unsigned char charset_index = 2;
const char *currentcharset;

struct termios orig_termios;

char *back_buffer = NULL;
size_t buffer_size = 0;

typedef struct {
    float x, y, z;
} Vector3D;

Vector3D *ray_lookup = NULL;
int lookup_cols = 0, lookup_rows = 0;

float alpha = 0.0f, beta = -90.0f;
float target_alpha = 0.0f, target_beta = 0.0f;

float sin_a = 0.0f, cos_a = 1.0f;
float sin_b = 0.0f, cos_b = 1.0f;

Vector3D pos = {128.0f, 128.0f, 40.0f};
Vector3D target_pos = {0.0f, 0.0f, 0.0f};

struct winsize global_w;
struct winsize render_w;

size_t frame_data_bytes, required_size;

typedef struct {
    int thread_id, start_y, end_y;
} ThreadWorker;

pthread_t threads[MAX_THREADS];
ThreadWorker workers[MAX_THREADS];
int num_cores = 32;
volatile sig_atomic_t keep_running = 1;

void cleanup() {
    printf("\033[?2026l\033[?25h\033[?7h\033[2J\033[H"); 
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    if (map.ptr) free(map.ptr); map.ptr = NULL;
    if (ray_lookup) free(ray_lookup); ray_lookup = NULL;
    if (back_buffer) free(back_buffer); back_buffer = NULL ;
}

void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

void bake_screen_rays(int cols, int rows) {
    if (cols == lookup_cols && rows == lookup_rows && ray_lookup != NULL) {
        return;
    }

    lookup_cols = cols;
    lookup_rows = rows;

    ray_lookup = (Vector3D *)realloc(ray_lookup, (size_t)cols * rows * sizeof(Vector3D));
    if (!ray_lookup) {
        cleanup();
        fprintf(stderr, "Heap allocation failed for ray_lookup\n");
        exit(1);
    }

    for (int y = 0; y < rows; y++) {
        float ray_beta = (float)(y - rows / 2) / (rows > 0 ? rows : 1) * deg_to_rad(45.0f);
        for (int x = 0; x < cols; x++) {
            float ray_alpha = (float)(x - cols / 2) / (cols > 0 ? cols : 1) * 2.0f * deg_to_rad(50.0f);
            size_t idx = (size_t)y * cols + x;

            ray_lookup[idx].x = sinf(ray_alpha) * cosf(ray_beta);
            ray_lookup[idx].y = cosf(ray_alpha) * cosf(ray_beta);
            ray_lookup[idx].z = sinf(ray_beta);
        }
    }
}

void update_terminal_size() {
    struct winsize current_w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &current_w) == -1) {
        return;
    }
    if (current_w.ws_col != render_w.ws_col || current_w.ws_row != render_w.ws_row) {
        render_w = current_w;
        
        bake_screen_rays(render_w.ws_col, render_w.ws_row);
        
        frame_data_bytes = ((size_t)(render_w.ws_col + 1) * render_w.ws_row) - 1;
        required_size = frame_data_bytes + PREFIX_LEN + 64;
        
        if (required_size > buffer_size) {
            buffer_size = required_size;
            char *new_buffer = realloc(back_buffer, buffer_size);
            if (!new_buffer) {
                cleanup();
                fprintf(stderr, "Buffer reallocation failed during resize\n");
                exit(1);
            }
            back_buffer = new_buffer;
        }

        printf("\033[2J\033[H"); 
        fflush(stdout);
    }
}

void reset_player_to_safe() {
    pos.x = (float)map.width / 2.0f;
    pos.y = (float)map.length / 2.0f;
    pos.z = (float)map.height / 2.0f;
    target_pos.x = 0.0f;
    target_pos.y = 0.0f;
    target_pos.z = 0.0f;
}

void *render_thread_strip(void *arg) {
    ThreadWorker *worker = (ThreadWorker *)arg;

    for (int y = worker->start_y; y >= worker->end_y; y--) {
        if (y < 0 || y >= render_w.ws_row) continue;

        size_t row_offset = PREFIX_LEN + (size_t)y * ((size_t)render_w.ws_col + 1);
        char *buf_ptr = back_buffer + row_offset;

        for (int x = 0; x < render_w.ws_col; x++) {
            size_t idx = (size_t)(render_w.ws_row - 1 - y) * render_w.ws_col + x;
            Vector3D ray = ray_lookup[idx];

            float t_vy = ray.y * cos_b - ray.z * sin_b;
            float c    = ray.y * sin_b + ray.z * cos_b;
            float a    = ray.x * cos_a - t_vy * sin_a;
            float b    = ray.x * sin_a + t_vy * cos_a;

            int i = 0;
            Vector3D sample = pos;
            for(i = 0; i < render_length; i++) {
                sample.x += a; sample.y += b; sample.z += c;
                if (sample.x < 0.0f) {
                    if (map.border_type & B_LEFT) break;
                    else if (map.border_type & B_RIGHT) {i = render_length; break;}
                    else { sample.x += (float)map.width; }

                } else if (sample.x >= (float)map.width) {
                    if (map.border_type & B_RIGHT) break;
                    else if (map.border_type & B_LEFT) {i = render_length; break;}
                    else { sample.x -= (float)map.width; }
                }

                if (sample.y < 0.0f) {
                    if (map.border_type & B_BACK) break;
                    else if (map.border_type & B_FRONT) {i = render_length; break;}
                    else { sample.y += (float)map.length; }

                } else if (sample.y >= (float)map.length) {
                    if (map.border_type & B_FRONT) break;
                    else if (map.border_type & B_BACK) {i = render_length; break;}
                    else { sample.y -= (float)map.length; }
                }

                if (sample.z < 0.0f) {
                    if (map.border_type & B_BOTTOM) break;
                    else if (map.border_type & B_TOP) {i = render_length; break;}
                    else { sample.z += (float)map.width; }

                } else if (sample.z >= (float)map.height) {
                    if (map.border_type & B_TOP) break;
                    else if (map.border_type & B_BOTTOM) {i = render_length; break;}
                    else { sample.z -= (float)map.height; }
                }

                if (get_map_voxel(sample.x, sample.y, sample.z) != 0) break;
            }
            *buf_ptr++ = currentcharset[i >> (7 - charset_index)];
        }
        if(y) *buf_ptr++ = '\n';
    }
    return NULL;
}

int main() {
    currentcharset = charset[charset_index];
    num_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores > MAX_THREADS) num_cores = MAX_THREADS;
    if (num_cores < 1) num_cores = 1;

    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(cleanup);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    printf("\033[?25l\033[2J\033[?7l");
    fflush(stdout);

    generate_random_map();
    srand((unsigned int)time(NULL));

    update_terminal_size();

    while (keep_running) {
        update_terminal_size();

        Vector3D forward = { .x = -sin_a * cos_b, .y = cos_a * cos_b, .z = sin_b };
        Vector3D strafe  = { .x = cos_a, .y = sin_a, .z = 0.0f };

        Vector3D wish_dir = {0.0f, 0.0f, 0.0f};

        while (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == '\033') {
                char seq[16]; int len = 0;
                while (len < 15 && read(STDIN_FILENO, &seq[len], 1) > 0) {
                    if (seq[len] == 'M' || seq[len] == 'm' || (seq[len] >= 'A' && seq[len] <= 'Z')) break;
                    len++;
                }
            }
            else {
                if (ch == 'w' || ch == 'W') { wish_dir.x += forward.x; wish_dir.y += forward.y; wish_dir.z += forward.z; }
                if (ch == 's' || ch == 'S') { wish_dir.x -= forward.x; wish_dir.y -= forward.y; wish_dir.z -= forward.z; }
                if (ch == 'a' || ch == 'A') { wish_dir.x -= strafe.x;  wish_dir.y -= strafe.y;  wish_dir.z -= strafe.z; }
                if (ch == 'd' || ch == 'D') { wish_dir.x += strafe.x;  wish_dir.y += strafe.y;  wish_dir.z += strafe.z; }

                if (ch == 'i' || ch == 'I') { target_beta += look_speed; if (target_beta > 89.0f)  target_beta = 89.0f; }
                if (ch == 'k' || ch == 'K') { target_beta -= look_speed; if (target_beta < -89.0f) target_beta = -89.0f; }
                if (ch == 'j' || ch == 'J') { target_alpha += look_speed; if (target_alpha > 360.0f)  { target_alpha -= 360.0f; alpha -= 360.0f; } }
                if (ch == 'l' || ch == 'L') { target_alpha -= look_speed; if (target_alpha < -360.0f) { target_alpha += 360.0f; alpha += 360.0f; } }

                if (ch == ' ') { wish_dir.z += 1.0f; }
                if (ch == 'x' || ch == 'X') { wish_dir.z -= 1.0f; }

                if (ch == 'r' || ch == 'R') {
                    generate_next_map();
                    reset_player_to_safe();
                }
                if (ch == 'q' || ch == 'Q') {
                    charset_index = (charset_index + 1) % 6;
                    currentcharset = charset[charset_index];
                    render_length = (~((unsigned char)0) << (7 - charset_index));
                }
            }
        }

        alpha += (target_alpha - alpha) * smoothing_factor;
        beta  += (target_beta - beta) * smoothing_factor;

        target_pos.x += (wish_dir.x * speed - target_pos.x) * smoothing_factor;
        target_pos.y += (wish_dir.y * speed - target_pos.y) * smoothing_factor;
        target_pos.z += (wish_dir.z * speed - target_pos.z) * smoothing_factor;

        pos.x += target_pos.x; pos.y += target_pos.y; pos.z += target_pos.z;

        float dim_x = (float)map.width; float dim_y = (float)map.length; float dim_z = (float)map.height;

        if ((map.border_type & B_LEFT) || (map.border_type & B_RIGHT)) {
            if (pos.x < 2.0f) { pos.x = 2.0f; target_pos.x = 0.0f; }
            if (pos.x > dim_x - 2.0f) { pos.x = dim_x - 2.0f; target_pos.x = 0.0f; }
        } else { pos.x = fmodf(pos.x, dim_x); if (pos.x < 0.0f) pos.x += dim_x; }

        if ((map.border_type & B_BACK) || (map.border_type & B_FRONT)) {
            if (pos.y < 2.0f) { pos.y = 2.0f; target_pos.y = 0.0f; }
            if (pos.y > dim_y - 2.0f) { pos.y = dim_y - 2.0f; target_pos.y = 0.0f; }
        } else { pos.y = fmodf(pos.y, dim_y); if (pos.y < 0.0f) pos.y += dim_y; }

        if ((map.border_type & B_BOTTOM) || (map.border_type & B_TOP)) {
            if (pos.z < 2.0f) { pos.z = 2.0f; target_pos.z = 0.0f; }
            if (pos.z > dim_z - 2.0f) { pos.z = dim_z - 2.0f; target_pos.z = 0.0f; }
        } else { pos.z = fmodf(pos.z, dim_z); if (pos.z < 0.0f) pos.z += dim_z; }

        sin_a = sinf(deg_to_rad(alpha));   cos_a = cosf(deg_to_rad(alpha));
        sin_b = sinf(deg_to_rad(beta));    cos_b = cosf(deg_to_rad(beta));

        memcpy(back_buffer, PREFIX_STR, PREFIX_LEN);
        size_t final_trailer_offset = PREFIX_LEN + frame_data_bytes;

        int rows_per_thread = render_w.ws_row / num_cores;
        for (int i = 0; i < num_cores; i++) {
            workers[i].thread_id = i;
            workers[i].start_y = render_w.ws_row - 1 - (i * rows_per_thread);
            workers[i].end_y = (i == num_cores - 1) ? 0 : render_w.ws_row - ((i + 1) * rows_per_thread);

            pthread_create(&threads[i], NULL, render_thread_strip, &workers[i]);
        }

        for (int i = 0; i < num_cores; i++) {
            pthread_join(threads[i], NULL);
        }

        write(STDOUT_FILENO, back_buffer, final_trailer_offset);

        usleep(10000);
    }

    cleanup();
    return 0;
}
