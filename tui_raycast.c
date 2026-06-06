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

const float speed = 3.5f;
const float look_speed = 4.0f;
const float smoothing_factor = 0.18f;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define map(value, fromLow, fromHigh, toLow, toHigh) (((value) - (fromLow)) * ((toHigh) - (toLow)) / ((fromHigh) - (fromLow)) + (toLow))
#define deg_to_rad(deg) ((deg) * M_PI / 180.0f)

#define MAP_SIZE 512
#define MAP_SIZE_SQ ((size_t)512 * 512)
#define MAX_THREADS 32

char *charset8  = "#*+=-:. ";
char *charset16 = "S&MW$B@%#*+=-:. ";
char *charset32 = "$#\\|)(1}{][?-_+~><i!lI;:,\"\n^`'. ";
char *charset64 = "$@B%8&ahkbdpqwmZO0QLCJUYXzcvunxrjft/\\|()1{}[]?-_+~<>i!lI;:,\"^`'. ";

unsigned char ch;
unsigned char generated_map_type = 5;
unsigned char charset_index = 0;
char * currentcharset;

unsigned char *mape = NULL;
struct termios orig_termios;

char *back_buffer = NULL;
char *front_buffer = NULL;
size_t buffer_size = 0;

typedef struct {
    float x;
    float y;
    float z;
} Vector3D;

Vector3D *ray_lookup = NULL;
int lookup_cols = 0;
int lookup_rows = 0;

float alpha = 0.0f, beta = -90.0f;
float target_alpha = 0.0f, target_beta = 0.0f;

float sin_a = 0.0f, cos_a = 1.0f;
float sin_b = 0.0f, cos_b = 1.0f;

Vector3D Pos = {256.0f, 256.0f, 40.0f};
Vector3D target_Pos = {256.0f, 256.0f, 40.0f};

struct winsize global_w;
struct winsize render_w;

typedef struct {
    int thread_id;
    int start_y;
    int end_y;
} ThreadWorker;

pthread_t threads[MAX_THREADS];
ThreadWorker workers[MAX_THREADS];
pthread_barrier_t barrier;
int num_cores = 32;
volatile sig_atomic_t keep_running = 1;

void cleanup() {
    printf("\033[?2026l\033[?25h\033[2J\033[H");
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    pthread_barrier_destroy(&barrier);
    if (mape) free(mape);
    if (back_buffer) free(back_buffer);
    if (front_buffer) free(front_buffer);
    if (ray_lookup) free(ray_lookup);
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
            size_t idx = (size_t)(rows - y - 1) * cols + x;

            ray_lookup[idx].x = sinf(ray_alpha) * cosf(ray_beta);
            ray_lookup[idx].y = cosf(ray_alpha) * cosf(ray_beta);
            ray_lookup[idx].z = sinf(ray_beta);
        }
    }
}

void *render_thread_strip(void *arg) {
    ThreadWorker *worker = (ThreadWorker *)arg;
    while(1) {
        pthread_barrier_wait(&barrier);

        if (!keep_running) break;

        const size_t prefix_len = 11;

        for (int y = worker->start_y; y >= worker->end_y; y--) {
            int screen_y_idx = render_w.ws_row - 1 - y;
            if (screen_y_idx < 0) screen_y_idx = 0;
            if (screen_y_idx >= render_w.ws_row) screen_y_idx = render_w.ws_row - 1;

            size_t row_offset = prefix_len + (size_t)screen_y_idx * ((size_t)render_w.ws_col + 1);
            char *buf_ptr = back_buffer + row_offset;

            for (int x = 0; x < render_w.ws_col; x++) {
                size_t idx = (size_t)screen_y_idx * render_w.ws_col + x;
                Vector3D ray = ray_lookup[idx];

                float t_vy = ray.y * cos_b - ray.z * sin_b;
                float c    = ray.y * sin_b + ray.z * cos_b;
                float a    = ray.x * cos_a - t_vy * sin_a;
                float b    = ray.x * sin_a + t_vy * cos_a;
                int charIndex = (1 << (charset_index + 3)) - 1;

                for(int i = 0; i < 256; i++) {
                    Vector3D sample = {
                        .x = Pos.x + (i * a),
                        .y = Pos.y + (i * b),
                        .z = Pos.z + (i * c)
                    };

                    if (sample.x < 0.0f || sample.x >= (float)MAP_SIZE ||
                        sample.y < 0.0f || sample.y >= (float)MAP_SIZE ||
                        sample.z < 0.0f || sample.z >= (float)MAP_SIZE) {
                        charIndex =  i >> (5 - charset_index);//map(i, 0, 256, PANEL_CHARSET_DEFAULT_LEN, 0);

                        break;
                    }

                    size_t flat_index = ((size_t)sample.x * MAP_SIZE_SQ) + ((size_t)sample.y * MAP_SIZE) + (size_t)sample.z;

                    if (mape[flat_index]) {
                        charIndex =  i >> (5 - charset_index);//map(i, 0, 256, PANEL_CHARSET_DEFAULT_LEN - 1, 0);
                        break;
                    }
                }
                *buf_ptr++ = currentcharset[charIndex];
            }
            *buf_ptr++ = '\n';
        }
        pthread_barrier_wait(&barrier);
    }
    return NULL;
}

void generate_random_map() {
    memset(mape, 0, (size_t)MAP_SIZE * MAP_SIZE * MAP_SIZE);
    for(int i = 0; i < MAP_SIZE_SQ; i++) {
        int rx = (int)(rand() % MAP_SIZE);
        int ry = (int)(rand() % MAP_SIZE);
        int rz = (int)(rand() % MAP_SIZE);
        mape[(rx * MAP_SIZE_SQ) + (ry * MAP_SIZE) + rz] = 1;
    }
}

void generate_cubes_map() {
    memset(mape, 0, (size_t)MAP_SIZE * MAP_SIZE * MAP_SIZE);
    for(int obj = 0; obj < 512; obj++) {
        int startX = rand() % (MAP_SIZE - 20);
        int startY = rand() % (MAP_SIZE - 20);
        int startZ = rand() % (MAP_SIZE - 20);
        int boxSize = 5 + (rand() % 15);

        for(int x = startX; x < startX + boxSize; x++) {
            for(int y = startY; y < startY + boxSize; y++) {
                for(int z = startZ; z < startZ + boxSize; z++) {
                    mape[((size_t)x * MAP_SIZE_SQ) + ((size_t)y * MAP_SIZE) + z] = 1;
                }
            }
        }
    }
}

void generate_pipenetwork_map() {
    memset(mape, 0, (size_t)MAP_SIZE * MAP_SIZE * MAP_SIZE);
    int num_pipes = 60;
    for (int p = 0; p < num_pipes; p++) {
        int x = rand() % MAP_SIZE;
        int y = rand() % MAP_SIZE;
        int z = rand() % MAP_SIZE;

        int dx = 0, dy = 0, dz = 0;
        int axis = rand() % 3;
        if (axis == 0) dx = 1;
        else if (axis == 1) dy = 1;
        else dz = 1;

        int length = 100 + (rand() % 200);
        int thickness = 2 + (rand() % 4);

        for (int step = 0; step < length; step++) {
            for (int tx = -thickness; tx <= thickness; tx++) {
                for (int ty = -thickness; ty <= thickness; ty++) {
                    for (int tz = -thickness; tz <= thickness; tz++) {
                        int px = x + tx;
                        int py = y + ty;
                        int pz = z + tz;

                        if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE && pz >= 0 && pz < MAP_SIZE) {
                            mape[((size_t)px * MAP_SIZE_SQ) + ((size_t)py * MAP_SIZE) + pz] = 1;
                        }
                    }
                }
            }
            x += dx; y += dy; z += dz;
        }
    }
}

void generate_wave_map() {
    memset(mape, 0, (size_t)MAP_SIZE * MAP_SIZE * MAP_SIZE);
    float frequency = 0.05f;
    float amplitude = 30.0f;
    float base_height = 256.0f;

    for (int x = 0; x < MAP_SIZE; x++) {
        for (int y = 0; y < MAP_SIZE; y++) {
            float z_float = base_height + sinf((float)x * frequency) * amplitude
                                        + cosf((float)y * frequency) * amplitude;
            int z = (int)z_float;

            for (int t = -2; t <= 2; t++) {
                int pz = z + t;
                if (pz >= 0 && pz < MAP_SIZE) {
                    mape[((size_t)x * MAP_SIZE_SQ) + ((size_t)y * MAP_SIZE) + pz] = 1;
                }
            }
        }
    }
}

void generate_cage_map() {
    memset(mape, 0, (size_t)MAP_SIZE * MAP_SIZE * MAP_SIZE);
    int grid_spacing = 64;
    int thickness = 2;

    for (int x = 0; x < MAP_SIZE; x++) {
        int x_mod = x % grid_spacing;
        int near_x_line = (x_mod <= thickness || x_mod >= grid_spacing - thickness);

        for (int y = 0; y < MAP_SIZE; y++) {
            int y_mod = y % grid_spacing;
            int near_y_line = (y_mod <= thickness || y_mod >= grid_spacing - thickness);

            for (int z = 0; z < MAP_SIZE; z++) {
                int z_mod = z % grid_spacing;
                int near_z_line = (z_mod <= thickness || z_mod >= grid_spacing - thickness);

                if ((near_x_line && near_y_line) ||
                    (near_y_line && near_z_line) ||
                    (near_x_line && near_z_line)) {
                    mape[((size_t)x * MAP_SIZE_SQ) + ((size_t)y * MAP_SIZE) + z] = 1;
                }
            }
        }
    }
}

void generate_city_map() {
    memset(mape, 0, (size_t)MAP_SIZE * MAP_SIZE * MAP_SIZE);

    int block_size = 48;
    int road_width = 8;

    for (int x = 0; x < MAP_SIZE; x++) {
        int is_road_x = (x % (block_size + road_width)) < road_width;

        for (int y = 0; y < MAP_SIZE; y++) {
            int is_road_y = (y % (block_size + road_width)) < road_width;

            if (is_road_x || is_road_y) {
                mape[((size_t)x * MAP_SIZE_SQ) + ((size_t)y * MAP_SIZE) + 0] = 1;
                continue;
            }

            int cell_x = x / (block_size + road_width);
            int cell_y = y / (block_size + road_width);

            unsigned int seed = (unsigned int)(cell_x * 73856093 ^ cell_y * 19349663);
            int building_height = 40 + (seed % 180);

            int local_x = x % (block_size + road_width) - road_width;
            int local_y = y % (block_size + road_width) - road_width;

            int padding = 4;
            if (local_x >= padding && local_x < (block_size - padding) &&
                local_y >= padding && local_y < (block_size - padding)) {

                for (int z = 0; z < building_height; z++) {
                    size_t flat_index = ((size_t)x * MAP_SIZE_SQ) + ((size_t)y * MAP_SIZE) + z;
                    mape[flat_index] = 1;
                }
            } else {
                mape[((size_t)x * MAP_SIZE_SQ) + ((size_t)y * MAP_SIZE) + 0] = 1;
            }
        }
    }
}

int main() {

    mape = calloc((size_t)MAP_SIZE * MAP_SIZE * MAP_SIZE, sizeof(unsigned char));
    if (!mape) return 1;
    currentcharset = charset8;
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

    printf("\033[?25l\033[2J");
    fflush(stdout);

    pthread_barrier_init(&barrier, NULL, num_cores + 1);

    srand((unsigned int)time(NULL));
    generate_random_map();

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &global_w);
    render_w = global_w;
    bake_screen_rays(render_w.ws_col, render_w.ws_row);

    int rows_per_thread = render_w.ws_row / num_cores;
    for (int i = 0; i < num_cores; i++) {
        workers[i].thread_id = i;
        workers[i].start_y = render_w.ws_row - 1 - (i * rows_per_thread);
        workers[i].end_y = (i == num_cores - 1) ? 0 : render_w.ws_row - ((i + 1) * rows_per_thread);
        pthread_create(&threads[i], NULL, render_thread_strip, &workers[i]);
    }

    while (keep_running) {
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &global_w);
        render_w = global_w;

        size_t frame_data_bytes = (size_t)(render_w.ws_col + 1) * render_w.ws_row;
        size_t required_size = frame_data_bytes + 64;
        if (required_size > buffer_size) {
            buffer_size = required_size;
            back_buffer = realloc(back_buffer, buffer_size);
            front_buffer = realloc(front_buffer, buffer_size);
        }

        bake_screen_rays(render_w.ws_col, render_w.ws_row);

        Vector3D forward = {
            .x = -sin_a * cos_b,
            .y =  cos_a * cos_b,
            .z =  sin_b
        };
        Vector3D strafe = {
            .x =  cos_a,
            .y =  sin_a,
            .z =  0.0f
        };

        while (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == '\033') {
                char seq[16]; int len = 0;
                while (len < 15 && read(STDIN_FILENO, &seq[len], 1) > 0) {
                    if (seq[len] == 'M' || seq[len] == 'm' || (seq[len] >= 'A' && seq[len] <= 'Z')) {
                        break;
                    }
                    len++;
                }
            }
            else {
                 if (ch == 'w' || ch == 'W') { target_Pos.x += forward.x * speed; target_Pos.y += forward.y * speed; target_Pos.z += forward.z * speed; }
                 if (ch == 's' || ch == 'S') { target_Pos.x -= forward.x * speed; target_Pos.y -= forward.y * speed; target_Pos.z -= forward.z * speed; }
                 if (ch == 'a' || ch == 'A') { target_Pos.x -= strafe.x * speed;  target_Pos.y -= strafe.y * speed;  target_Pos.z -= strafe.z * speed; }
                 if (ch == 'd' || ch == 'D') { target_Pos.x += strafe.x * speed;  target_Pos.y += strafe.y * speed;  target_Pos.z += strafe.z * speed; }

                 if (ch == 'i' || ch == 'I') { target_beta += look_speed; }
                 if (ch == 'k' || ch == 'K') { target_beta -= look_speed; }
                 if (ch == 'j' || ch == 'J') { target_alpha += look_speed;}
                 if (ch == 'l' || ch == 'L') { target_alpha -= look_speed;}

                 if (ch == ' ') { target_Pos.z += speed; }
                 if (ch == 'x' || ch == 'X') { target_Pos.z -= speed; }
                 if (ch == 'r' || ch == 'R') {
                    switch (generated_map_type = (generated_map_type + 1) % 6) {
                        case 0: generate_cubes_map(); break;
                        case 1: generate_pipenetwork_map(); break;
                        case 2: generate_wave_map(); break;
                        case 3: generate_cage_map(); break;
                        case 4: generate_city_map(); break;
                        default: generate_random_map(); break;
                    }
                }
                if (ch == 'q' || ch == 'Q') {
                    switch (charset_index = (charset_index + 1) % 4) {
                        case 1: currentcharset = charset16; break;
                        case 2: currentcharset = charset32; break;
                        case 3: currentcharset = charset64; break;
                        default: currentcharset = charset8; break;
                    }
                }
            }
        }

        if (target_beta > 89.0f)  target_beta = 89.0f;
        if (target_beta < -89.0f) target_beta = -89.0f;

        alpha += (target_alpha - alpha) * smoothing_factor;
        beta  += (target_beta - beta) * smoothing_factor;

        Pos.x += (target_Pos.x - Pos.x) * smoothing_factor;
        Pos.y += (target_Pos.y - Pos.y) * smoothing_factor;
        Pos.z += (target_Pos.z - Pos.z) * smoothing_factor;

        if (target_alpha > 360.0f)  { target_alpha -= 360.0f; alpha -= 360.0f; }
        if (target_alpha < -360.0f) { target_alpha += 360.0f; alpha += 360.0f; }
        if (target_beta > 360.0f)  { target_beta -= 360.0f; beta -= 360.0f; }
        if (target_beta < -360.0f) { target_beta += 360.0f; beta += 360.0f; }

        if (target_Pos.x < 2) target_Pos.x = 2; if (target_Pos.x > 512) target_Pos.x = 510;
        if (target_Pos.y < 2) target_Pos.y = 2; if (target_Pos.y > 512) target_Pos.y = 510;
        if (target_Pos.z < 2) target_Pos.z = 2; if (target_Pos.z > 510) target_Pos.z = 510;
        if (Pos.x < 2.0f) Pos.x = 2.0f; if (Pos.x > 510.0f) Pos.x = 510.0f;
        if (Pos.y < 2.0f) Pos.y = 2.0f; if (Pos.y > 510.0f) Pos.y = 510.0f;
        if (Pos.z < 2.0f) Pos.z = 2.0f; if (Pos.z > 510.0f) Pos.z = 510.0f;

        sin_a = sinf(deg_to_rad(alpha));   cos_a = cosf(deg_to_rad(alpha));
        sin_b = sinf(deg_to_rad(beta));    cos_b = cosf(deg_to_rad(beta));

        rows_per_thread = render_w.ws_row / num_cores;
        for (int i = 0; i < num_cores; i++) {
            workers[i].start_y = render_w.ws_row - 1 - (i * rows_per_thread);
            workers[i].end_y = (i == num_cores - 1) ? 0 : render_w.ws_row - ((i + 1) * rows_per_thread);
        }

        memcpy(back_buffer, "\033[?2026h\033[H", 11);
        size_t final_trailer_offset = 11 + frame_data_bytes;
        memcpy(back_buffer + final_trailer_offset, "\033[?2026l", 8);

        pthread_barrier_wait(&barrier);
        pthread_barrier_wait(&barrier);

        size_t packet_size = final_trailer_offset + 8;
        if (memcmp(back_buffer, front_buffer, packet_size) != 0) {
            memcpy(front_buffer, back_buffer, packet_size);
            write(STDOUT_FILENO, front_buffer, packet_size);
        }
        usleep(10000);
    }

    pthread_barrier_wait(&barrier);
    for (int i = 0; i < num_cores; i++) {
        pthread_join(threads[i], NULL);
    }

    cleanup();
    return 0;
}
