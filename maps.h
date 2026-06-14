#define MAP_SIZE 256

#define B_LEFT   (1 << 0)
#define B_RIGHT  (1 << 1)
#define B_BOTTOM (1 << 2)
#define B_TOP    (1 << 3)
#define B_FLOOR  (1 << 4)
#define B_CEIL   (1 << 5)

struct {
    unsigned char *ptr;
    int width, length, height;
    unsigned char border_type;
} map = {NULL, 0, 0, 0, 0};

unsigned char generated_map_type = 5;

int preparemap(int width, int length, int height, unsigned char border_type) {
    if (map.ptr) {
        free(map.ptr);
        map.ptr = NULL;
    }

    size_t total_bits = (size_t)width * length * height;
    map.ptr = calloc((total_bits >> 3) + 1, sizeof(unsigned char));
    if (!map.ptr) return 1;

    map.width = width;
    map.length = length;
    map.height = height;
    map.border_type = border_type;

    return 0;
}

void put_map_voxel(int x, int y, int z) {
    if (x < 0 || x >= map.width || y < 0 || y >= map.length || z < 0 || z >= map.height) return;
    size_t flat_index = ((size_t)x * map.length * map.height) + ((size_t)y * map.height) + (size_t)z;
    map.ptr[flat_index >> 3] |= (1 << (flat_index % 8));
}

void generate_random_map() {
    preparemap(MAP_SIZE, MAP_SIZE, MAP_SIZE, 0);
    for(int i = 0; i < 2048; i++) {
        int rx = rand() % map.width;
        int ry = rand() % map.length;
        int rz = rand() % map.height;
        put_map_voxel(rx, ry, rz);
    }
}

void generate_cubes_map() {
    preparemap(MAP_SIZE, MAP_SIZE, MAP_SIZE, 0);
    for(int obj = 0; obj < 256; obj++) {
        int startX = rand() % (MAP_SIZE - 20);
        int startY = rand() % (MAP_SIZE - 20);
        int startZ = rand() % (MAP_SIZE - 20);
        int boxSize = 5 + (rand() % 15);

        for(int x = startX; x < startX + boxSize; x++) {
            for(int y = startY; y < startY + boxSize; y++) {
                for(int z = startZ; z < startZ + boxSize; z++) {
                    put_map_voxel(x, y, z);
                }
            }
        }
    }
}

void generate_pipenetwork_map() {
    preparemap(MAP_SIZE, MAP_SIZE, MAP_SIZE, (B_FLOOR | B_CEIL));
    int num_pipes = 128;

    for (int p = 0; p < num_pipes; p++) {
        int x = rand() % MAP_SIZE;
        int y = rand() % MAP_SIZE;
        int z = rand() % MAP_SIZE;

        int dx = 0, dy = 0, dz = 0;
        int axis = rand() % 3;
        if (axis == 0) dx = 1;
        else if (axis == 1) dy = 1;
        else dz = 1;

        int pipe_length = 100 + (rand() % 200);
        int thickness = 2 + (rand() % 4);

        for (int step = 0; step < pipe_length; step++) {
            for (int tx = (dx ? 0 : -thickness); tx <= (dx ? 0 : thickness); tx++) {
                for (int ty = (dy ? 0 : -thickness); ty <= (dy ? 0 : thickness); ty++) {
                    for (int tz = (dz ? 0 : -thickness); tz <= (dz ? 0 : thickness); tz++) {
                        int px = x + tx;
                        int py = y + ty;
                        int pz = z + tz;
                        put_map_voxel(px, py, pz);
                    }
                }
            }
            x += dx; y += dy; z += dz;
        }
    }
}

void generate_wave_map() {
    preparemap(MAP_SIZE, MAP_SIZE, MAP_SIZE, (B_LEFT | B_RIGHT | B_FLOOR | B_CEIL));
    float frequency = 0.0256f * M_PI;
    float amplitude = 30.0f;
    float base_height = 128.0f;

    float x_waves[MAP_SIZE];
    for (int x = 0; x < MAP_SIZE; x++) {
        x_waves[x] = base_height + sinf((float)x * frequency) * amplitude;
    }

    float y_waves[MAP_SIZE];
    for (int y = 0; y < MAP_SIZE; y++) {
        y_waves[y] = cosf((float)y * frequency) * amplitude;
    }

    for (int x = 0; x < MAP_SIZE; x++) {
        float base_x = x_waves[x];
        for (int y = 0; y < MAP_SIZE; y++) {
            int z_center = (int)(base_x + y_waves[y]);
            int z_start = z_center - 2;
            int z_end = z_center + 2;

            if (z_start < 0) z_start = 0;
            if (z_end >= MAP_SIZE) z_end = MAP_SIZE - 1;

            for (int pz = z_start; pz <= z_end; pz++) {
                put_map_voxel(x, y, pz);
            }
        }
    }
}

void generate_cage_map() {
    int grid_spacing = 64;
    int thickness = 2;
    preparemap(grid_spacing, grid_spacing, grid_spacing, 0);
    
    for (int x = 0; x < grid_spacing; x++) {
        int x_mod = x % grid_spacing;
        int near_x_line = (x_mod <= thickness || x_mod >= grid_spacing - thickness);

        for (int y = 0; y < grid_spacing; y++) {
            int y_mod = y % grid_spacing;
            int near_y_line = (y_mod <= thickness || y_mod >= grid_spacing - thickness);

            for (int z = 0; z < grid_spacing; z++) {
                int z_mod = z % grid_spacing;
                int near_z_line = (z_mod <= thickness || z_mod >= grid_spacing - thickness);

                if ((near_x_line && near_y_line) ||
                    (near_y_line && near_z_line) ||
                    (near_x_line && near_z_line)) {
                    put_map_voxel(x, y, z);
                }
            }
        }
    }
}

void generate_city_map() {
    preparemap(MAP_SIZE, MAP_SIZE, 128, B_BOTTOM);
    int block_size = 48;
    int road_width = 8;

    for (int x = 0; x < MAP_SIZE; x++) {
        int is_road_x = (x % (block_size + road_width)) < road_width;

        for (int y = 0; y < MAP_SIZE; y++) {
            int is_road_y = (y % (block_size + road_width)) < road_width;

            if (is_road_x || is_road_y) {
                put_map_voxel(x, y, 0);
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
                    put_map_voxel(x, y, z);
                }
            } else {
                put_map_voxel(x, y, 0);
            }
        }
    }
}

void generate_next_map() {
    switch (generated_map_type = (generated_map_type + 1) % 6) {
        case 0: generate_cubes_map(); break;
        case 1: generate_pipenetwork_map(); break;
        case 2: generate_wave_map(); break;
        case 3: generate_cage_map(); break;
        case 4: generate_city_map(); break;
        default: generate_random_map(); break;
    }
}