#define MAP_SIZE 256

#define B_LEFT   (1 << 0)
#define B_RIGHT  (1 << 1)
#define B_BACK   (1 << 2)
#define B_FRONT  (1 << 3)
#define B_BOTTOM (1 << 4)
#define B_TOP    (1 << 5)

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
    map.ptr = (unsigned char *)calloc((total_bits >> 3) + 1, sizeof(unsigned char));
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
    map.ptr[flat_index >> 3] |= (1 << (flat_index & 7));
}

unsigned char get_map_voxel(int x, int y, int z) {
    if (x < 0 || x >= map.width || y < 0 || y >= map.length || z < 0 || z >= map.height) return 0;
    size_t flat_index = ((size_t)x * map.length * map.height) + ((size_t)y * map.height) + (size_t)z;
    return map.ptr[flat_index >> 3] & (1 << (flat_index & 7));
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
    preparemap(MAP_SIZE, MAP_SIZE, MAP_SIZE, (B_TOP | B_BOTTOM));
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
                        put_map_voxel(x + tx, y + ty, z + tz);
                    }
                }
            }
            x += dx; y += dy; z += dz;
        }
    }
}

void generate_wave_map() {
    preparemap(MAP_SIZE, MAP_SIZE, MAP_SIZE, (B_LEFT | B_RIGHT | B_BACK | B_FRONT));
    float frequency = 0.05f;
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
    int thickness = 4;
    int grid_spacing = 64;
    preparemap(grid_spacing, grid_spacing, grid_spacing, 0);

    for (int i = 0; i < grid_spacing; i++) {
        for (int j = 0; j < thickness; j++) {
            for (int k = 0; k < thickness; k++) {
                put_map_voxel(i, j, k);
                put_map_voxel(k, i, j);
                put_map_voxel(j, k, i);
            }
        }
    }
}

void generate_city_map() {
    const int map_max_h = 128;
    preparemap(MAP_SIZE, MAP_SIZE, map_max_h, B_BOTTOM);
    int block_size = 48;
    int road_width = 8;
    int cycle_period = block_size + road_width;

    for (int x = 0; x < MAP_SIZE; x++) {
        int local_x = x % cycle_period;
        int is_road_x = local_x < road_width;
        int cell_x = x / cycle_period;

        for (int y = 0; y < MAP_SIZE; y++) {
            int local_y = y % cycle_period;
            int is_road_y = local_y < road_width;

            if (is_road_x || is_road_y) {
                put_map_voxel(x, y, 0);
                continue;
            }

            int cell_y = y / cycle_period;

            unsigned int seed = (unsigned int)(cell_x * 73856093 ^ cell_y * 19349663);
            int building_height = 40 + (seed % 90);

            if (building_height > map_max_h) building_height = map_max_h;

            int active_x = local_x - road_width;
            int active_y = local_y - road_width;

            int padding = 4;
            if (active_x >= padding && active_x < (block_size - padding) &&
                active_y >= padding && active_y < (block_size - padding)) {

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