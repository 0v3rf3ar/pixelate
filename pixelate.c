#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#define PIXELATE_VERSION_VALUE "v1.0.1"
#define PIXELATE_DEVELOPER_VALUE "s3p"
#define PIXELATE_GITHUB_VALUE "https://github.com/0v3rf3ar/pixelate"
#define PIXELATE_COPYRIGHT_VALUE "Copyright (c) 2026 s3p. All rights reserved."

static const char *PIXELATE_VERSION = PIXELATE_VERSION_VALUE;
static const char *PIXELATE_DEVELOPER = PIXELATE_DEVELOPER_VALUE;
static const char *PIXELATE_GITHUB = PIXELATE_GITHUB_VALUE;

/* Keep human-readable ownership metadata in every binary, including stripped
   release builds. Windows builds also expose these fields through VERSIONINFO. */
#if defined(_MSC_VER)
#pragma section(".pixelate", read)
__declspec(allocate(".pixelate"))
#elif defined(__APPLE__)
__attribute__((used, section("__DATA,__pixelate")))
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((used, section(".pixelate_metadata")))
#endif
static const char PIXELATE_BINARY_METADATA[] =
    "Product=Pixelate\0"
    "Version=" PIXELATE_VERSION_VALUE "\0"
    "Developer=" PIXELATE_DEVELOPER_VALUE "\0"
    "GitHub=" PIXELATE_GITHUB_VALUE "\0"
    "Copyright=" PIXELATE_COPYRIGHT_VALUE "\0"
    "License=Proprietary - All Rights Reserved\0";
static const char *ASCII_CHARS = " .:-=+*M%@";
static const unsigned char ASV1_MAGIC[4] = {'A', 'S', 'V', '1'};
static const unsigned char ASV2_MAGIC[4] = {'A', 'S', 'V', '2'};
static const unsigned char ASV3_MAGIC[4] = {'A', 'S', 'V', '3'};
static volatile sig_atomic_t playback_interrupted = 0;

static void handle_playback_signal(int signal_number) {
    (void)signal_number;
    playback_interrupted = 1;
}

typedef struct {
    int invert;
    int color;
    int block;
    int encode_video;
    int include_audio;
    unsigned char excluded[256];
    const char *input_path;
    const char *output_path;
    int first_value;
    int second_value;
    int fps;
    int automatic_size;
    int fit_size;
} Options;

typedef struct {
#ifdef _WIN32
    HANDLE handle;
    DWORD original_mode;
#endif
    int active;
} TerminalOutput;

/* Windows consoles require VT processing before ANSI RGB and screen controls
   are interpreted. macOS and other Unix terminals support these sequences
   directly, so their setup is intentionally a no-op. */
static int terminal_output_start(TerminalOutput *output) {
    output->active = 0;
#ifdef _WIN32
    output->handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output->handle == INVALID_HANDLE_VALUE || output->handle == NULL)
        return 0;
    if (!GetConsoleMode(output->handle, &output->original_mode)) {
        /* Redirected output is not a console; preserve ANSI sequences for the
           receiving terminal or file instead of treating this as an error. */
        return GetFileType(output->handle) != FILE_TYPE_CHAR;
    }
    if (!SetConsoleMode(output->handle,
                        output->original_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return 0;
    output->active = 1;
#endif
    return 1;
}

static void terminal_output_stop(TerminalOutput *output) {
#ifdef _WIN32
    if (output->active)
        SetConsoleMode(output->handle, output->original_mode);
#else
    (void)output;
#endif
    output->active = 0;
}

static void print_usage(const char *program) {
    printf("Pixelate %s - images and videos in your terminal\n\n", PIXELATE_VERSION);
    printf("Developer: %s\n", PIXELATE_DEVELOPER);
    printf("GitHub:    %s\n\n", PIXELATE_GITHUB);
    printf("Usage:\n");
    printf("  %s image <file> [options]\n", program);
    printf("  %s video encode <file> <output.asv> [options]\n", program);
    printf("  %s video play <file.asv> [options]\n\n", program);
    printf("Display options (image and video play):\n");
    printf("  -c, --color          Use the source colors on ASCII characters\n");
    printf("  -b, --block          Draw colored background blocks instead of characters\n");
    printf("  -i, --invert         Reverse ASCII brightness or block colors\n");
    printf("  --remove <chars>     Replace listed ASCII characters with spaces\n\n");
    printf("Sizing:\n");
    printf("  --size auto          Largest size that preserves the source aspect ratio\n");
    printf("  --size fit           Fill the terminal; video playback follows resizes live\n");
    printf("  --size WxH           Output image or encoded ASV frame dimensions\n");
    printf("  --size native        Play an ASV at its stored dimensions\n\n");
    printf("Video encode options:\n");
    printf("  --fps <12-30>        Output frame rate (default: 24)\n");
    printf("  -a, --audio          Include the source audio in the ASV file\n");
    printf("  FFmpeg handles GIF, MOV, MKV, MP4, WebM, AVI, and other formats.\n\n");
    printf("Other options:\n");
    printf("  -h, --help           Show this help\n");
    printf("  -v, --version        Print the program version\n\n");
    printf("Examples:\n");
    printf("  %s image photo.jpg --size auto --color\n", program);
    printf("  %s video encode clip.mkv clip.asv --size auto --fps 24\n", program);
    printf("  %s video play clip.asv --block --size fit\n", program);
}

static int terminal_size(int *columns, int *rows) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE terminal = INVALID_HANDLE_VALUE;
    if (!GetConsoleScreenBufferInfo(handle, &info)) {
        terminal = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (terminal == INVALID_HANDLE_VALUE ||
            !GetConsoleScreenBufferInfo(terminal, &info)) {
            if (terminal != INVALID_HANDLE_VALUE) CloseHandle(terminal);
            return 0;
        }
        CloseHandle(terminal);
    }
    *columns = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
#else
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
        !size.ws_col || !size.ws_row) {
        int terminal = open("/dev/tty", O_RDONLY);
        if (terminal < 0 || ioctl(terminal, TIOCGWINSZ, &size) != 0 ||
            !size.ws_col || !size.ws_row) {
            if (terminal >= 0) close(terminal);
            return 0;
        }
        close(terminal);
    }
    *columns = size.ws_col;
    *rows = size.ws_row;
#endif
    return *columns > 0 && *rows > 1;
}

/* Terminal cells are approximately twice as tall as they are wide. */
static int fit_cells(int media_width, int media_height, int block_mode,
                     int *columns, int *rows) {
    int terminal_columns, terminal_rows;
    double width_limit, height_limit, fitted_width, fitted_height;
    if (media_width <= 0 || media_height <= 0 ||
        !terminal_size(&terminal_columns, &terminal_rows)) return 0;
    terminal_rows -= 1;
    (void)block_mode;
    /* Every renderer emits exactly one terminal cell per sample. */
    width_limit = terminal_columns;
    height_limit = terminal_rows;
    fitted_width = width_limit;
    fitted_height = fitted_width * media_height / media_width / 2.0;
    if (fitted_height > height_limit) {
        fitted_height = height_limit;
        fitted_width = fitted_height * media_width / media_height * 2.0;
    }
    *columns = (int)fitted_width;
    *rows = (int)fitted_height;
    if (*columns < 1) *columns = 1;
    if (*rows < 1) *rows = 1;
    return 1;
}

/* ASV dimensions already describe terminal cells, so no cell-aspect correction. */
static int fit_asv_grid(int frame_width, int frame_height, int *columns, int *rows) {
    int terminal_columns, terminal_rows;
    double fitted_width, fitted_height;
    if (frame_width <= 0 || frame_height <= 0 ||
        !terminal_size(&terminal_columns, &terminal_rows)) return 0;
    fitted_width = terminal_columns;
    fitted_height = fitted_width * frame_height / frame_width;
    if (fitted_height > terminal_rows) {
        fitted_height = terminal_rows;
        fitted_width = fitted_height * frame_width / frame_height;
    }
    *columns = (int)fitted_width;
    *rows = (int)fitted_height;
    if (*columns < 1) *columns = 1;
    if (*rows < 1) *rows = 1;
    return 1;
}

static int has_suffix(const char *text, const char *suffix) {
    size_t text_length = strlen(text), suffix_length = strlen(suffix);
    return text_length >= suffix_length &&
           strcmp(text + text_length - suffix_length, suffix) == 0;
}

static char grayscale_to_ascii(int gray, const Options *options) {
    size_t count = strlen(ASCII_CHARS);
    size_t index = (size_t)gray * (count - 1) / 255;
    char result;
    if (options->invert) index = count - 1 - index;
    result = ASCII_CHARS[index];
    return options->excluded[(unsigned char)result] ? ' ' : result;
}

static int rgb_to_gray(int r, int g, int b) {
    return (299 * r + 587 * g + 114 * b) / 1000;
}

static void render_cell(int r, int g, int b, int a, const Options *options) {
    char character = a == 0 ? ' ' :
        grayscale_to_ascii(rgb_to_gray(r, g, b), options);
    if (options->block && a != 0) {
        if (options->invert) {
            r = 255 - r;
            g = 255 - g;
            b = 255 - b;
        }
        printf("\x1b[48;2;%d;%d;%dm ", r, g, b);
    } else if (options->color && character != ' ') {
        printf("\x1b[38;2;%d;%d;%dm%c", r, g, b, character);
    } else {
        putchar(character);
    }
}

static void finish_row(const Options *options) {
    if (options->color || options->block) fputs("\x1b[0m", stdout);
    putchar('\n');
}

static int parse_dimensions(const char *text, int *first, int *second) {
    char trailing;
    return sscanf(text, "%dx%d%c", first, second, &trailing) == 2 &&
           *first > 0 && *second > 0;
}

static int parse_fps(const char *text, int *fps) {
    char trailing;
    return sscanf(text, "%d%c", fps, &trailing) == 1 && *fps >= 12 && *fps <= 30;
}

static int set_size(const char *text, Options *options, int allow_native) {
    if (!strcmp(text, "auto")) options->automatic_size = 1;
    else if (!strcmp(text, "fit")) options->fit_size = 1;
    else if (allow_native && !strcmp(text, "native")) { }
    else if (!parse_dimensions(text, &options->first_value, &options->second_value)) {
        fprintf(stderr, "Invalid size '%s'. Use auto, fit, native, or WxH.\n", text);
        return 0;
    }
    return 1;
}

static int parse_display_option(int argc, char **argv, int *index, Options *options) {
    const char *arg = argv[*index];
    if (!strcmp(arg, "-i") || !strcmp(arg, "--invert")) options->invert = 1;
    else if (!strcmp(arg, "-c") || !strcmp(arg, "--color")) options->color = 1;
    else if (!strcmp(arg, "-b") || !strcmp(arg, "--block")) options->block = 1;
    else if (!strcmp(arg, "--remove")) {
        if (++*index >= argc) { fprintf(stderr, "--remove requires characters.\n"); return 0; }
        for (const unsigned char *p = (const unsigned char *)argv[*index]; *p; ++p)
            options->excluded[*p] = 1;
    } else return 0;
    return 1;
}

static int parse_subcommand(int argc, char **argv, Options *options) {
    int start, allow_native = 0;
    memset(options, 0, sizeof(*options));
    options->fps = 24;
    options->automatic_size = 1;
    if (!strcmp(argv[1], "image")) {
        if (argc < 3) { fprintf(stderr, "image requires an input file.\n"); return -1; }
        options->input_path = argv[2]; start = 3;
    } else if (!strcmp(argv[1], "video")) {
        if (argc < 4) { fprintf(stderr, "Use 'video encode' or 'video play'.\n"); return -1; }
        if (!strcmp(argv[2], "encode")) {
            if (argc < 5) { fprintf(stderr, "video encode requires input and output files.\n"); return -1; }
            options->encode_video = 1; options->input_path = argv[3];
            options->output_path = argv[4]; start = 5;
            if (!has_suffix(options->output_path, ".asv")) {
                fprintf(stderr, "Output filename must end in .asv.\n"); return -1;
            }
        } else if (!strcmp(argv[2], "play")) {
            options->input_path = argv[3]; start = 4; allow_native = 1;
            if (!has_suffix(options->input_path, ".asv")) {
                fprintf(stderr, "video play requires an .asv file.\n"); return -1;
            }
        } else { fprintf(stderr, "Unknown video command '%s'. Use encode or play.\n", argv[2]); return -1; }
    } else return 0;

    for (int i = start; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]); return 1;
        } else if (!strcmp(argv[i], "--size")) {
            if (++i >= argc) { fprintf(stderr, "--size requires auto, fit, native, or WxH.\n"); return -1; }
            options->automatic_size = options->fit_size = 0;
            if (!set_size(argv[i], options, allow_native)) return -1;
        } else if (!strcmp(argv[i], "--fps") && options->encode_video) {
            if (++i >= argc || !parse_fps(argv[i], &options->fps)) {
                fprintf(stderr, "--fps must be between 12 and 30.\n"); return -1;
            }
        } else if ((!strcmp(argv[i], "-a") || !strcmp(argv[i], "--audio")) &&
                   options->encode_video) {
            options->include_audio = 1;
        } else if (!parse_display_option(argc, argv, &i, options)) {
            fprintf(stderr, "Unknown option '%s'. Try --help.\n", argv[i]); return -1;
        }
    }
    return 2;
}

static int parse_options(int argc, char **argv, Options *options) {
    const char *positionals[5];
    int count = 0;
    int subcommand;
    if (argc > 1 && (!strcmp(argv[1], "image") || !strcmp(argv[1], "video"))) {
        subcommand = parse_subcommand(argc, argv, options);
        return subcommand == 2 ? 0 : subcommand;
    }
    memset(options, 0, sizeof(*options));

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            print_usage(argv[0]); return 1;
        }
        if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) {
            printf("Pixelate %s\n", PIXELATE_VERSION);
            printf("Developer: %s\n", PIXELATE_DEVELOPER);
            printf("GitHub:    %s\n", PIXELATE_GITHUB);
            return 1;
        }
        if (!strcmp(arg, "-i") || !strcmp(arg, "--invert")) options->invert = 1;
        else if (!strcmp(arg, "-c") || !strcmp(arg, "--color")) options->color = 1;
        else if (!strcmp(arg, "-b") || !strcmp(arg, "--block")) options->block = 1;
        else if (!strcmp(arg, "--encode-video")) options->encode_video = 1;
        else if (!strcmp(arg, "--audio")) options->include_audio = 1;
        else if (arg[0] == '-' && arg[1] && arg[1] != '-') {
            const unsigned char *p = (const unsigned char *)arg + 1;
            while (*p) {
                if (*p == 'i') options->invert = 1;
                else if (*p == 'c') options->color = 1;
                else if (*p == 'b') options->block = 1;
                else if (*p == 'a') options->include_audio = 1;
                else options->excluded[*p] = 1;
                ++p;
            }
        } else if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg); return -1;
        } else if (count < 5) positionals[count++] = arg;
        else { fprintf(stderr, "Too many arguments.\n"); return -1; }
    }

    if (options->encode_video) {
        if (count != 4 || (strcmp(positionals[2], "auto") != 0 &&
            !parse_dimensions(positionals[2], &options->first_value,
                              &options->second_value))) {
            print_usage(argv[0]); return -1;
        }
        options->automatic_size = strcmp(positionals[2], "auto") == 0;
        options->input_path = positionals[0];
        options->output_path = positionals[1];
        if (!has_suffix(options->output_path, ".asv")) {
            fprintf(stderr, "Video output must use the .asv extension.\n"); return -1;
        }
        {
            char trailing;
            if (sscanf(positionals[3], "%d%c", &options->fps, &trailing) != 1 ||
                options->fps < 12 || options->fps > 30) {
                fprintf(stderr, "FPS must be between 12 and 30.\n"); return -1;
            }
        }
    } else if (count == 1 && has_suffix(positionals[0], ".asv")) {
        options->input_path = positionals[0];
    } else if (count == 2 && (strcmp(positionals[1], "auto") == 0 ||
               parse_dimensions(positionals[1], &options->first_value,
                                &options->second_value))) {
        options->input_path = positionals[0];
        options->automatic_size = strcmp(positionals[1], "auto") == 0;
    } else {
        print_usage(argv[0]); return -1;
    }
    return 0;
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8),
        (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return fwrite(bytes, 1, 4, file) == 4;
}

static int read_u32(FILE *file, uint32_t *value) {
    unsigned char bytes[4];
    if (fread(bytes, 1, 4, file) != 4) return 0;
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return 1;
}

static int write_u16(FILE *file, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
    return fwrite(bytes, 1, 2, file) == 2;
}

static int read_u16(FILE *file, uint16_t *value) {
    unsigned char bytes[2];
    if (fread(bytes, 1, 2, file) != 2) return 0;
    *value = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
    return 1;
}

static uint16_t rgb_to_565(const unsigned char *pixel) {
    return (uint16_t)(((uint16_t)(pixel[0] >> 3) << 11) |
                      ((uint16_t)(pixel[1] >> 2) << 5) | (pixel[2] >> 3));
}

static void rgb_from_565(uint16_t color, unsigned char *pixel) {
    unsigned int r = (color >> 11) & 31u, g = (color >> 5) & 63u, b = color & 31u;
    pixel[0] = (unsigned char)((r << 3) | (r >> 2));
    pixel[1] = (unsigned char)((g << 2) | (g >> 4));
    pixel[2] = (unsigned char)((b << 3) | (b >> 2));
}

/* ASV2: high-bit runs copy the previous frame; low-bit runs contain RGB565 pixels. */
static int write_compressed_frame(FILE *output, const unsigned char *rgb,
                                  uint16_t *previous, size_t pixels) {
    size_t position = 0;
    while (position < pixels) {
        uint16_t color = rgb_to_565(rgb + position * 3u);
        int unchanged = color == previous[position];
        size_t start = position, run;
        while (position < pixels && position - start < 0x7fffu) {
            color = rgb_to_565(rgb + position * 3u);
            if ((color == previous[position]) != unchanged) break;
            ++position;
        }
        run = position - start;
        if (!write_u16(output, (uint16_t)(run | (unchanged ? 0x8000u : 0u)))) return 0;
        if (!unchanged) {
            for (size_t index = start; index < position; ++index) {
                color = rgb_to_565(rgb + index * 3u);
                if (!write_u16(output, color)) return 0;
                previous[index] = color;
            }
        }
    }
    return 1;
}

static int read_compressed_frame(FILE *input, unsigned char *rgb,
                                 uint16_t *previous, size_t pixels) {
    size_t position = 0;
    while (position < pixels) {
        uint16_t control;
        size_t run;
        if (!read_u16(input, &control)) return 0;
        run = control & 0x7fffu;
        if (!run || run > pixels - position) return 0;
        if (control & 0x8000u) {
            for (size_t i = 0; i < run; ++i)
                rgb_from_565(previous[position + i], rgb + (position + i) * 3u);
        } else {
            for (size_t i = 0; i < run; ++i) {
                uint16_t color;
                if (!read_u16(input, &color)) return 0;
                previous[position + i] = color;
                rgb_from_565(color, rgb + (position + i) * 3u);
            }
        }
        position += run;
    }
    return 1;
}

static int shell_quote(const char *input, char *output, size_t size) {
#ifdef _WIN32
    char quote = '"';
#else
    char quote = '\'';
#endif
    size_t used = 0;
    if (size < 3) return 0;
    output[used++] = quote;
    for (; *input; ++input) {
#ifndef _WIN32
        if (*input == '\'') {
            const char escaped[] = "'\\''";
            if (used + 4 >= size) return 0;
            memcpy(output + used, escaped, 4); used += 4; continue;
        }
#else
        if (*input == '"') return 0;
#endif
        if (used + 2 >= size) return 0;
        output[used++] = *input;
    }
    output[used++] = quote; output[used] = '\0';
    return 1;
}

static int probe_video_size(const char *quoted_path, int *width, int *height) {
    char command[4096], line[128];
    FILE *pipe;
    int status;
    if (snprintf(command, sizeof(command),
        "ffprobe -v error -select_streams v:0 -show_entries stream=width,height "
        "-of csv=s=x:p=0 %s", quoted_path) >= (int)sizeof(command)) return 0;
#ifdef _WIN32
    pipe = popen(command, "r");
#else
    pipe = popen(command, "r");
#endif
    if (!pipe) return 0;
    if (!fgets(line, sizeof(line), pipe)) { pclose(pipe); return 0; }
    status = pclose(pipe);
    return status == 0 && sscanf(line, "%dx%d", width, height) == 2 &&
           *width > 0 && *height > 0;
}

static int append_audio_stream(FILE *output, const char *quoted_path,
                               uint32_t *audio_offset, uint32_t *audio_size) {
    char command[4096];
    unsigned char buffer[16384];
    FILE *pipe;
    long offset;
    uint64_t total = 0;
    size_t count;
    if (fseek(output, 0, SEEK_END) != 0 || (offset = ftell(output)) < 0 ||
        (uint64_t)offset > UINT32_MAX) return 0;
    if (snprintf(command, sizeof(command),
        "ffmpeg -v error -i %s -map 0:a:0 -vn -c:a libmp3lame -q:a 4 -f mp3 -",
        quoted_path) >= (int)sizeof(command)) return 0;
#ifdef _WIN32
    pipe = popen(command, "rb");
#else
    pipe = popen(command, "r");
#endif
    if (!pipe) return 0;
    while ((count = fread(buffer, 1, sizeof(buffer), pipe)) != 0) {
        if (total + count > UINT32_MAX || fwrite(buffer, 1, count, output) != count) {
            pclose(pipe); return 0;
        }
        total += count;
    }
    if (pclose(pipe) != 0 || total == 0) return 0;
    *audio_offset = (uint32_t)offset;
    *audio_size = (uint32_t)total;
    return 1;
}

static int encode_video(const Options *options) {
    char quoted[2048], command[4096];
    FILE *pipe = NULL, *output = NULL;
    unsigned char *frame = NULL;
    uint16_t *previous = NULL;
    int output_width = options->first_value, output_height = options->second_value;
    size_t frame_size;
    uint32_t frame_count = 0;
    uint32_t audio_offset = 0, audio_size = 0;
    int result = EXIT_FAILURE;

    if (!shell_quote(options->input_path, quoted, sizeof(quoted))) {
        fprintf(stderr, "Video path or dimensions are too large.\n"); return EXIT_FAILURE;
    }
    if (options->fit_size) {
        if (!terminal_size(&output_width, &output_height)) {
            fprintf(stderr, "Cannot determine terminal dimensions for fit sizing.\n");
            return EXIT_FAILURE;
        }
        --output_height;
    } else if (options->automatic_size) {
        int media_width, media_height;
        if (!probe_video_size(quoted, &media_width, &media_height) ||
            !fit_cells(media_width, media_height, options->block,
                       &output_width, &output_height)) {
            fprintf(stderr, "Cannot determine video or terminal dimensions for auto sizing.\n");
            return EXIT_FAILURE;
        }
    }
    frame_size = (size_t)output_width * (size_t)output_height * 3u;
    if (frame_size == 0 || frame_size > SIZE_MAX / 2) {
        fprintf(stderr, "Video dimensions are too large.\n"); return EXIT_FAILURE;
    }
    if (snprintf(command, sizeof(command),
        "ffmpeg -v error -i %s -vf fps=%d,scale=%d:%d:flags=area -f rawvideo -pix_fmt rgb24 -",
        quoted, options->fps, output_width, output_height) >= (int)sizeof(command)) {
        fprintf(stderr, "FFmpeg command is too long.\n"); return EXIT_FAILURE;
    }
    output = fopen(options->output_path, "wb+");
    frame = (unsigned char *)malloc(frame_size);
    previous = (uint16_t *)malloc(frame_size / 3u * sizeof(*previous));
    if (!output || !frame || !previous) { fprintf(stderr, "Cannot create ASV output.\n"); goto cleanup; }
    memset(previous, 0xff, frame_size / 3u * sizeof(*previous));
    if (fwrite(options->include_audio ? ASV3_MAGIC : ASV2_MAGIC, 1, 4, output) != 4 ||
        !write_u32(output, (uint32_t)output_width) ||
        !write_u32(output, (uint32_t)output_height) ||
        !write_u32(output, (uint32_t)options->fps) || !write_u32(output, 0) ||
        (options->include_audio &&
         (!write_u32(output, 0) || !write_u32(output, 0)))) goto cleanup;
#ifdef _WIN32
    pipe = popen(command, "rb");
#else
    pipe = popen(command, "r");
#endif
    if (!pipe) { fprintf(stderr, "Could not start FFmpeg. Is it installed?\n"); goto cleanup; }
    while (fread(frame, 1, frame_size, pipe) == frame_size) {
        if (!write_compressed_frame(output, frame, previous, frame_size / 3u)) goto cleanup;
        ++frame_count;
    }
    if (pclose(pipe) != 0) { pipe = NULL; fprintf(stderr, "FFmpeg failed.\n"); goto cleanup; }
    pipe = NULL;
    if (frame_count == 0 || fseek(output, 16, SEEK_SET) != 0 || !write_u32(output, frame_count)) {
        fprintf(stderr, "No complete video frames were encoded.\n"); goto cleanup;
    }
    if (options->include_audio) {
        if (!append_audio_stream(output, quoted, &audio_offset, &audio_size)) {
            fprintf(stderr, "Could not encode source audio. Ensure it has an audio stream "
                            "and FFmpeg includes MP3 support.\n");
            goto cleanup;
        }
        if (fseek(output, 20, SEEK_SET) != 0 || !write_u32(output, audio_offset) ||
            !write_u32(output, audio_size)) goto cleanup;
    }
    printf("Created %s: %u frames, %dx%d, %d FPS%s\n", options->output_path,
           frame_count, output_width, output_height, options->fps,
           options->include_audio ? ", audio included" : "");
    result = EXIT_SUCCESS;
cleanup:
    if (pipe) pclose(pipe);
    if (output && fclose(output) != 0) result = EXIT_FAILURE;
    free(frame);
    free(previous);
    if (result != EXIT_SUCCESS) remove(options->output_path);
    return result;
}

enum PlaybackKey { PLAYBACK_KEY_NONE, PLAYBACK_KEY_ESCAPE, PLAYBACK_KEY_SPACE,
                   PLAYBACK_KEY_LEFT, PLAYBACK_KEY_RIGHT,
                   PLAYBACK_KEY_UP, PLAYBACK_KEY_DOWN };

typedef struct {
#ifndef _WIN32
    struct termios original;
#endif
    int active;
} PlaybackInput;

static int playback_input_start(PlaybackInput *input) {
    input->active = 0;
#ifdef _WIN32
    input->active = 1;
    return 1;
#else
    struct termios raw;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &input->original) != 0) return 0;
    raw = input->original;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return 0;
    input->active = 1;
    return 1;
#endif
}

static void playback_input_stop(PlaybackInput *input) {
    if (!input->active) return;
#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSANOW, &input->original);
#endif
    input->active = 0;
}

static enum PlaybackKey playback_key(int wait_ms) {
#ifdef _WIN32
    DWORD start = GetTickCount();
    while (!_kbhit()) {
        if (wait_ms <= 0 || GetTickCount() - start >= (DWORD)wait_ms) return PLAYBACK_KEY_NONE;
        Sleep(1);
    }
    {
        int key = _getch();
        if (key == 0x1b) return PLAYBACK_KEY_ESCAPE;
        if (key == ' ') return PLAYBACK_KEY_SPACE;
        if (key == 0 || key == 0xe0) {
            key = _getch();
            if (key == 75) return PLAYBACK_KEY_LEFT;
            if (key == 77) return PLAYBACK_KEY_RIGHT;
            if (key == 72) return PLAYBACK_KEY_UP;
            if (key == 80) return PLAYBACK_KEY_DOWN;
        }
    }
#else
    fd_set read_set;
    struct timeval timeout;
    unsigned char bytes[16];
    int count;
    FD_ZERO(&read_set); FD_SET(STDIN_FILENO, &read_set);
    timeout.tv_sec = wait_ms / 1000; timeout.tv_usec = (wait_ms % 1000) * 1000;
    count = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
    if (count <= 0) return PLAYBACK_KEY_NONE;
    count = (int)read(STDIN_FILENO, bytes, sizeof(bytes));
    for (int i = 0; i < count; ++i) {
        if (bytes[i] == ' ') return PLAYBACK_KEY_SPACE;
        if (bytes[i] == 0x1b &&
            !(i + 2 < count && bytes[i + 1] == '['))
            return PLAYBACK_KEY_ESCAPE;
        if (bytes[i] == 0x1b && i + 2 < count && bytes[i + 1] == '[') {
            if (bytes[i + 2] == 'D') return PLAYBACK_KEY_LEFT;
            if (bytes[i + 2] == 'C') return PLAYBACK_KEY_RIGHT;
            if (bytes[i + 2] == 'A') return PLAYBACK_KEY_UP;
            if (bytes[i + 2] == 'B') return PLAYBACK_KEY_DOWN;
        }
    }
#endif
    return PLAYBACK_KEY_NONE;
}

static double playback_time(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
}

typedef struct {
    char path[1024];
    int active;
    int volume_level;
#ifdef _WIN32
    PROCESS_INFORMATION process;
#else
    pid_t pid;
#endif
} AudioPlayer;

static int extract_audio(FILE *asv, uint32_t offset, uint32_t size,
                         AudioPlayer *audio) {
    unsigned char buffer[16384];
    FILE *output = NULL;
    uint32_t remaining = size;
#ifdef _WIN32
    char temporary_directory[MAX_PATH];
    if (!GetTempPathA((DWORD)sizeof(temporary_directory), temporary_directory) ||
        !GetTempFileNameA(temporary_directory, "pxa", 0, audio->path)) return 0;
    output = fopen(audio->path, "wb");
#else
    int descriptor;
    strcpy(audio->path, "/tmp/pixelate-audio-XXXXXX");
    descriptor = mkstemp(audio->path);
    if (descriptor >= 0) output = fdopen(descriptor, "wb");
    if (descriptor >= 0 && !output) close(descriptor);
#endif
    if (!output || fseek(asv, (long)offset, SEEK_SET) != 0) goto failure;
    while (remaining) {
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t count = fread(buffer, 1, wanted, asv);
        if (count != wanted || fwrite(buffer, 1, count, output) != count) goto failure;
        remaining -= (uint32_t)count;
    }
    if (fclose(output) != 0) { output = NULL; goto failure; }
    return 1;
failure:
    if (output) fclose(output);
    if (audio->path[0]) remove(audio->path);
    audio->path[0] = '\0';
    return 0;
}

static int audio_start(AudioPlayer *audio, double position) {
    char seconds[64], volume[16];
    if (!audio->path[0]) return 1;
    if (position < 0.0) position = 0.0;
    snprintf(seconds, sizeof(seconds), "%.3f", position);
    snprintf(volume, sizeof(volume), "%d", audio->volume_level * 10);
#ifdef _WIN32
    STARTUPINFOA startup;
    char command[4096];
    memset(&startup, 0, sizeof(startup));
    memset(&audio->process, 0, sizeof(audio->process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (snprintf(command, sizeof(command),
        "ffplay -v error -nodisp -autoexit -volume %s -ss %s \"%s\"",
        volume, seconds, audio->path) >= (int)sizeof(command) ||
        !CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &audio->process)) return 0;
    CloseHandle(audio->process.hThread);
#else
    audio->pid = fork();
    if (audio->pid < 0) return 0;
    if (audio->pid == 0) {
        FILE *null_file = fopen("/dev/null", "r+");
        if (null_file) {
            dup2(fileno(null_file), STDIN_FILENO);
            dup2(fileno(null_file), STDOUT_FILENO);
            dup2(fileno(null_file), STDERR_FILENO);
        }
        execlp("ffplay", "ffplay", "-v", "error", "-nodisp", "-autoexit",
               "-volume", volume, "-ss", seconds, audio->path, (char *)NULL);
        _exit(127);
    }
#endif
    audio->active = 1;
    return 1;
}

static void audio_stop(AudioPlayer *audio) {
    if (!audio->active) return;
#ifdef _WIN32
    TerminateProcess(audio->process.hProcess, 0);
    WaitForSingleObject(audio->process.hProcess, 3000);
    CloseHandle(audio->process.hProcess);
#else
    kill(audio->pid, SIGTERM);
    while (waitpid(audio->pid, NULL, 0) < 0 && errno == EINTR) { }
#endif
    audio->active = 0;
}

static int audio_restart(AudioPlayer *audio, double position) {
    audio_stop(audio);
    return audio_start(audio, position);
}

static void audio_cleanup(AudioPlayer *audio) {
    audio_stop(audio);
    if (audio->path[0]) remove(audio->path);
    audio->path[0] = '\0';
}

static enum PlaybackKey wait_for_playback_key(double deadline) {
    while (!playback_interrupted) {
        double remaining = deadline - playback_time();
        int milliseconds;
        if (remaining <= 0.0) return PLAYBACK_KEY_NONE;
        milliseconds = (int)(remaining * 1000.0);
        if (milliseconds > 20) milliseconds = 20;
        if (milliseconds < 1) milliseconds = 1;
        {
            enum PlaybackKey key = playback_key(milliseconds);
            if (key != PLAYBACK_KEY_NONE) return key;
        }
    }
    return PLAYBACK_KEY_NONE;
}

static void video_output_size(const Options *options, uint32_t frame_width,
                              uint32_t frame_height, int *output_width,
                              int *output_height) {
    *output_width = (int)frame_width;
    *output_height = (int)frame_height;
    if (options->fit_size && terminal_size(output_width, output_height)) {
        --*output_height;
    } else if (options->automatic_size && terminal_size(output_width, output_height)) {
        if (!fit_asv_grid((int)frame_width, (int)frame_height,
                          output_width, output_height)) {
            *output_width = (int)frame_width;
            *output_height = (int)frame_height;
        }
    }
    if (*output_width < 1) *output_width = 1;
    if (*output_height < 1) *output_height = 1;
}

static int play_video(const Options *options) {
    FILE *file = fopen(options->input_path, "rb");
    unsigned char magic[4], *frame = NULL;
    uint16_t *previous = NULL;
    uint32_t width, height, fps, frames;
    uint32_t audio_offset = 0, audio_size = 0;
    long video_data_offset = 20;
    size_t frame_size;
    int previous_output_width = 0, previous_output_height = 0;
    int result = EXIT_FAILURE;
    int playback_state_active = 0;
    PlaybackInput input = {0};
    uint16_t **checkpoints = NULL;
    long *checkpoint_offsets = NULL;
    uint32_t checkpoint_span, checkpoint_count;
    uint32_t current = 0;
    double next_frame_time = 0.0;
    int paused = 0;
    int compressed = 0;
    AudioPlayer audio = {0};
#ifndef _WIN32
    struct sigaction interrupt_action, old_interrupt_action;
    struct sigaction terminate_action, old_terminate_action;
#endif
    if (!file) { fprintf(stderr, "Cannot open %s\n", options->input_path); return result; }
    audio.volume_level = 10;
    if (fread(magic, 1, 4, file) != 4 ||
        (memcmp(magic, ASV1_MAGIC, 4) && memcmp(magic, ASV2_MAGIC, 4) &&
         memcmp(magic, ASV3_MAGIC, 4)) ||
        !read_u32(file, &width) || !read_u32(file, &height) ||
        !read_u32(file, &fps) || !read_u32(file, &frames) || !width || !height ||
        fps < 12 || fps > 30 || width > SIZE_MAX / height / 3u) {
        fprintf(stderr, "Invalid or unsupported ASV file.\n"); goto cleanup;
    }
    compressed = !memcmp(magic, ASV2_MAGIC, 4) || !memcmp(magic, ASV3_MAGIC, 4);
    if (!memcmp(magic, ASV3_MAGIC, 4)) {
        video_data_offset = 28;
        if (!read_u32(file, &audio_offset) || !read_u32(file, &audio_size) ||
            !audio_offset || !audio_size || audio_offset < (uint32_t)video_data_offset ||
            !extract_audio(file, audio_offset, audio_size, &audio) ||
            fseek(file, video_data_offset, SEEK_SET) != 0) {
            fprintf(stderr, "Invalid or damaged ASV3 audio stream.\n"); goto cleanup;
        }
    }
    frame_size = (size_t)width * height * 3u;
    frame = (unsigned char *)malloc(frame_size);
    if (compressed) {
        previous = (uint16_t *)malloc(frame_size / 3u * sizeof(*previous));
        if (previous) memset(previous, 0xff, frame_size / 3u * sizeof(*previous));
    }
    if (!frame || (compressed && !previous)) {
        fprintf(stderr, "Not enough memory for video frame.\n"); goto cleanup;
    }
    checkpoint_span = fps * 5u;
    checkpoint_count = 1u + (frames - 1u) / checkpoint_span;
    if (previous) {
        checkpoints = (uint16_t **)calloc(checkpoint_count, sizeof(*checkpoints));
        checkpoint_offsets = (long *)calloc(checkpoint_count, sizeof(*checkpoint_offsets));
        if (!checkpoints || !checkpoint_offsets) {
            fprintf(stderr, "Not enough memory for seek checkpoints.\n"); goto cleanup;
        }
    }
    playback_interrupted = 0;
#ifdef _WIN32
    signal(SIGINT, handle_playback_signal);
    signal(SIGTERM, handle_playback_signal);
#else
    memset(&interrupt_action, 0, sizeof(interrupt_action));
    memset(&terminate_action, 0, sizeof(terminate_action));
    interrupt_action.sa_handler = handle_playback_signal;
    terminate_action.sa_handler = handle_playback_signal;
    sigemptyset(&interrupt_action.sa_mask);
    sigemptyset(&terminate_action.sa_mask);
    sigaction(SIGINT, &interrupt_action, &old_interrupt_action);
    sigaction(SIGTERM, &terminate_action, &old_terminate_action);
#endif
    /* Alternate screen, hidden cursor, and no autowrap prevent terminal scrolling. */
    fputs("\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l\x1b[?7l", stdout);
    playback_state_active = 1;
    playback_input_start(&input);
    if (!audio_start(&audio, 0.0)) {
        fprintf(stderr, "Cannot start ffplay for ASV audio. Is it installed?\n");
        goto cleanup;
    }
    next_frame_time = playback_time();
    while (current < frames) {
        if (previous && current % checkpoint_span == 0) {
            uint32_t slot = current / checkpoint_span;
            if (!checkpoints[slot]) {
                long offset = ftell(file);
                checkpoints[slot] = (uint16_t *)malloc(frame_size / 3u * sizeof(**checkpoints));
                if (checkpoints[slot] && offset >= 0) {
                    memcpy(checkpoints[slot], previous,
                           frame_size / 3u * sizeof(**checkpoints));
                    checkpoint_offsets[slot] = offset;
                } else {
                    free(checkpoints[slot]); checkpoints[slot] = NULL;
                }
            }
        }
        if ((!compressed &&
             fread(frame, 1, frame_size, file) != frame_size) ||
            (compressed &&
             !read_compressed_frame(file, frame, previous, frame_size / 3u))) {
            fprintf(stderr, "\nASV file ended before its declared frame count.\n"); goto cleanup;
        }
        {
            int redraw;
            do {
                int output_width, output_height;
                video_output_size(options, width, height, &output_width, &output_height);
                redraw = 0;
                if (output_width != previous_output_width ||
                    output_height != previous_output_height) {
                    fputs("\x1b[2J", stdout);
                    previous_output_width = output_width;
                    previous_output_height = output_height;
                }
                fputs("\x1b[H", stdout);
                for (int y = 0; y < output_height && !playback_interrupted; ++y) {
                    uint32_t source_y = (uint32_t)((uint64_t)y * height /
                                                   (uint32_t)output_height);
                    int live_width, live_height;
                    video_output_size(options, width, height, &live_width, &live_height);
                    if (live_width != output_width || live_height != output_height) {
                        redraw = 1;
                        break;
                    }
                    printf("\x1b[%d;1H", y + 1);
                    for (int x = 0; x < output_width; ++x) {
                        uint32_t source_x = (uint32_t)((uint64_t)x * width /
                                                       (uint32_t)output_width);
                        const unsigned char *pixel = frame +
                            ((size_t)source_y * width + source_x) * 3u;
                        render_cell(pixel[0], pixel[1], pixel[2], 255, options);
                    }
                    if (options->color || options->block) fputs("\x1b[0m", stdout);
                }
                if (redraw) {
                    fputs("\x1b[0m\x1b[2J\x1b[H", stdout);
                    fflush(stdout);
                }
            } while (redraw && !playback_interrupted);
        }
        fflush(stdout);
        if (playback_interrupted) break;
        next_frame_time += 1.0 / (double)fps;
        for (;;) {
            enum PlaybackKey key = paused ? playback_key(50) :
                wait_for_playback_key(next_frame_time);
            if (playback_interrupted) break;
            if (key == PLAYBACK_KEY_ESCAPE) {
                playback_interrupted = 1;
                break;
            }
            if (key == PLAYBACK_KEY_SPACE) {
                paused = !paused;
                if (paused) {
                    audio_stop(&audio);
                } else {
                    if (!audio_start(&audio, (double)current / fps)) {
                        fprintf(stderr, "\nCannot resume ASV audio.\n"); goto cleanup;
                    }
                    next_frame_time = playback_time();
                }
                continue;
            }
            if (key == PLAYBACK_KEY_UP || key == PLAYBACK_KEY_DOWN) {
                int new_level = audio.volume_level +
                    (key == PLAYBACK_KEY_UP ? 1 : -1);
                if (new_level < 1) new_level = 1;
                if (new_level > 10) new_level = 10;
                if (new_level != audio.volume_level) {
                    audio.volume_level = new_level;
                    if (!paused && audio.path[0] &&
                        !audio_restart(&audio, (double)current / fps)) {
                        fprintf(stderr, "\nCannot change ASV audio volume.\n");
                        goto cleanup;
                    }
                }
                continue;
            }
            if (key == PLAYBACK_KEY_LEFT || key == PLAYBACK_KEY_RIGHT) {
                uint32_t target = key == PLAYBACK_KEY_LEFT ?
                    (current > checkpoint_span ? current - checkpoint_span : 0) :
                    (current + checkpoint_span < frames ? current + checkpoint_span : frames - 1u);
                if (target <= current) {
                    if (previous) {
                        uint32_t slot = target / checkpoint_span;
                        while (slot > 0 && !checkpoints[slot]) --slot;
                        if (checkpoints[slot]) {
                            memcpy(previous, checkpoints[slot],
                                   frame_size / 3u * sizeof(*previous));
                            if (fseek(file, checkpoint_offsets[slot], SEEK_SET) != 0)
                                goto cleanup;
                            current = slot * checkpoint_span;
                        } else {
                            memset(previous, 0xff, frame_size / 3u * sizeof(*previous));
                            if (fseek(file, video_data_offset, SEEK_SET) != 0) goto cleanup;
                            current = 0;
                        }
                    } else {
                        if (fseek(file, video_data_offset +
                                  (long)((size_t)target * frame_size), SEEK_SET) != 0)
                            goto cleanup;
                        current = target;
                    }
                } else {
                    ++current;
                }
                while (current < target) {
                    if ((!previous && fread(frame, 1, frame_size, file) != frame_size) ||
                        (previous && !read_compressed_frame(file, frame, previous,
                                                            frame_size / 3u)))
                        goto cleanup;
                    ++current;
                }
                if (!paused && !audio_restart(&audio, (double)target / fps)) {
                    fprintf(stderr, "\nCannot seek ASV audio.\n"); goto cleanup;
                }
                next_frame_time = playback_time();
                break;
            }
            if (!paused && key == PLAYBACK_KEY_NONE) { ++current; break; }
        }
    }
    result = EXIT_SUCCESS;
cleanup:
    audio_cleanup(&audio);
    playback_input_stop(&input);
    if (playback_state_active) {
        /* Restore wrap, styling, cursor, and the original screen on every exit. */
        fputs("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l", stdout); fflush(stdout);
#ifdef _WIN32
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
#else
        sigaction(SIGINT, &old_interrupt_action, NULL);
        sigaction(SIGTERM, &old_terminate_action, NULL);
#endif
    }
    if (checkpoints) {
        for (uint32_t i = 0; i < checkpoint_count; ++i) free(checkpoints[i]);
    }
    free(checkpoints); free(checkpoint_offsets);
    free(previous); free(frame); fclose(file); return result;
}

static int render_image(Options *options) {
    int width, height, source_channels;
    unsigned char *image = stbi_load(options->input_path, &width, &height,
                                     &source_channels, 4);
    if (!image) {
        fprintf(stderr, "Failed to load image: %s (%s)\n", options->input_path,
                stbi_failure_reason()); return EXIT_FAILURE;
    }
    {
        int output_columns = options->first_value;
        int output_rows = options->second_value;
        if (options->automatic_size || options->fit_size) {
            if (options->fit_size) {
                if (!terminal_size(&output_columns, &output_rows)) {
                    fprintf(stderr, "Cannot determine terminal dimensions for fit sizing.\n");
                    stbi_image_free(image); return EXIT_FAILURE;
                }
                --output_rows;
            } else if (!fit_cells(width, height, options->block,
                                  &output_columns, &output_rows)) {
                fprintf(stderr, "Cannot determine terminal dimensions for auto sizing.\n");
                stbi_image_free(image); return EXIT_FAILURE;
            }
        }
        for (int y = 0; y < output_rows; ++y) {
            int source_y = (int)((int64_t)y * height / output_rows);
            for (int x = 0; x < output_columns; ++x) {
                int source_x = (int)((int64_t)x * width / output_columns);
                const unsigned char *pixel = image +
                    (((size_t)source_y * width + (size_t)source_x) * 4u);
                render_cell(pixel[0], pixel[1], pixel[2], pixel[3], options);
            }
            finish_row(options);
        }
        stbi_image_free(image); return EXIT_SUCCESS;
    }
}

int main(int argc, char **argv) {
    Options options;
    TerminalOutput terminal_output = {0};
    int result;
    int parsed = parse_options(argc, argv, &options);
    if (parsed) return parsed < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    if (options.encode_video) return encode_video(&options);
    if (!terminal_output_start(&terminal_output)) {
        fprintf(stderr, "Cannot enable ANSI terminal output. On Windows, use "
                        "Windows Terminal or a console with VT support.\n");
        return EXIT_FAILURE;
    }
    result = has_suffix(options.input_path, ".asv") ?
        play_video(&options) : render_image(&options);
    terminal_output_stop(&terminal_output);
    return result;
}
