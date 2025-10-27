#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>

typedef unsigned char byte;

#define SPRITE_VERSION 2
#define IDSPRITEHEADER (('P' << 24) + ('S' << 16) + ('D' << 8) + 'I')
#define MAX_PATH_SIZE 4096
#define INITIAL_BUFFER_SIZE 0x100000
#define INITIAL_MAX_FRAMES 1000
#define INITIAL_TOKEN_SIZE 1024
#define PALETTE_SIZE 256

typedef enum
{
    ST_SYNC = 0,
    ST_RAND
} synctype_t;
typedef enum
{
    SPR_SINGLE = 0,
    SPR_GROUP
} spriteframetype_t;

typedef struct
{
    int ident;
    int version;
    int type;
    int texFormat;
    float boundingradius;
    int width;
    int height;
    int numframes;
    float beamlength;
    synctype_t synctype;
} dsprite_t;

typedef struct
{
    int origin[2];
    int width;
    int height;
} dspriteframe_t;

typedef struct
{
    int numframes;
} dspritegroup_t;

typedef struct
{
    float interval;
} dspriteinterval_t;

typedef struct
{
    spriteframetype_t type;
} dspriteframetype_t;

typedef struct
{
    spriteframetype_t type;
    void *pdata;
    float interval;
    int numgroupframes;
} spritepackage_t;

#define SPR_VP_PARALLEL_UPRIGHT 0
#define SPR_FACING_UPRIGHT 1
#define SPR_VP_PARALLEL 2
#define SPR_ORIENTED 3
#define SPR_VP_PARALLEL_ORIENTED 4

#define SPR_NORMAL 0
#define SPR_ADDITIVE 1
#define SPR_INDEXALPHA 2
#define SPR_ALPHTEST 3

static dsprite_t sprite;
static byte *byteimage, *lbmpalette;
static int byteimagewidth, byteimageheight;
static byte *lumpbuffer, *plump;
static char *spritedir;
static char *spriteoutname;
static char *cli_output_name;
static bool cli_output_consumed;
static int framesmaxs[2];
static int framecount;
static spritepackage_t *frames;
static int max_frames = INITIAL_MAX_FRAMES;
static size_t buffer_size = INITIAL_BUFFER_SIZE;
static bool do16bit = true;
static char *token;
static size_t token_size = INITIAL_TOKEN_SIZE;
static char *scriptbuffer;
static char *scriptptr;
static char *scriptend;
static byte *original_palette = NULL;
static bool palette_established = false;

static void error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

static void *safe_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (!ptr)
        error("Memory allocation failed");
    return ptr;
}

static void *safe_realloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr)
        error("Memory reallocation failed");
    return new_ptr;
}

static FILE *safe_open_read(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f)
        error("Could not open %s: %s", filename, strerror(errno));
    return f;
}

static FILE *safe_open_write(const char *filename)
{
    FILE *f = fopen(filename, "wb");
    if (!f)
        error("Could not create %s: %s", filename, strerror(errno));
    return f;
}

static void safe_read(FILE *f, void *buffer, int count)
{
    if (fread(buffer, 1, count, f) != (size_t)count)
    {
        error("File read failure");
    }
}

static void safe_write(FILE *f, void *buffer, int count)
{
    if (fwrite(buffer, 1, count, f) != (size_t)count)
    {
        error("File write failure");
    }
}

static bool is_absolute_path(const char *path)
{
    if (!path || !path[0])
        return false;

    if (path[0] == '/' || path[0] == '\\')
        return true;

    if (strlen(path) >= 2 && path[1] == ':' && isalpha((unsigned char)path[0]))
        return true;

    return false;
}

static void ensure_token_capacity(size_t needed)
{
    if (needed > token_size)
    {
        token_size = needed * 2;
        token = safe_realloc(token, token_size);
    }
}

static bool get_token(bool crossline)
{
    char *token_p;

    if (token == NULL)
    {
        token = safe_malloc(token_size);
    }

skip_whitespace:
    if (!scriptptr || scriptptr >= scriptend)
    {
        return false;
    }

    char c = *scriptptr++;

    if (c == '\n')
    {
        if (!crossline)
        {
            scriptptr--;
            return false;
        }
        goto skip_whitespace;
    }

    if (c <= ' ')
        goto skip_whitespace;

    if (c == '/' && scriptptr < scriptend && *scriptptr == '/')
    {
        if (!crossline)
        {
            scriptptr--;
            return false;
        }
        while (scriptptr < scriptend && *scriptptr != '\n')
        {
            scriptptr++;
        }
        goto skip_whitespace;
    }

    scriptptr--;

    token_p = token;

    if (c == '"')
    {
        scriptptr++;
        while (scriptptr < scriptend)
        {
            c = *scriptptr++;
            if (c == '"')
            {
                *token_p = 0;
                return true;
            }
            if ((token_p - token) >= (ptrdiff_t)(token_size - 1))
            {
                size_t pos = token_p - token;
                ensure_token_capacity(token_size * 2);
                token_p = token + pos;
            }
            *token_p++ = c;
        }
        error("EOF inside quoted token");
    }

    do
    {
        c = *scriptptr++;
        if (c <= 32)
            break;
        if ((token_p - token) >= (ptrdiff_t)(token_size - 1))
        {
            size_t pos = token_p - token;
            ensure_token_capacity(token_size * 2);
            token_p = token + pos;
        }
        *token_p++ = c;
        if (scriptptr >= scriptend)
            break;
    } while (true);

    if (scriptptr < scriptend)
        scriptptr--;

    *token_p = 0;
    return true;
}

static void start_script_parse(const char *filename)
{
    FILE *f = safe_open_read(filename);
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    scriptbuffer = safe_malloc(length + 1);
    safe_read(f, scriptbuffer, length);
    scriptbuffer[length] = 0;
    fclose(f);

    scriptptr = scriptbuffer;
    scriptend = scriptbuffer + length;
}

static void end_script_parse(void)
{
    if (scriptbuffer)
    {
        free(scriptbuffer);
        scriptbuffer = NULL;
        scriptptr = NULL;
        scriptend = NULL;
    }
}

static int little_long(int l)
{
    byte b1 = l & 255;
    byte b2 = (l >> 8) & 255;
    byte b3 = (l >> 16) & 255;
    byte b4 = (l >> 24) & 255;
    return ((int)b1 << 0) + ((int)b2 << 8) + ((int)b3 << 16) + ((int)b4 << 24);
}

static float little_float(float l)
{
    union
    {
        float f;
        byte b[4];
    } in, out;
    in.f = l;
    out.b[0] = in.b[0];
    out.b[1] = in.b[1];
    out.b[2] = in.b[2];
    out.b[3] = in.b[3];
    return out.f;
}

static void ensure_buffer_capacity(size_t needed)
{
    if (needed > buffer_size)
    {
        size_t current_offset = plump - lumpbuffer;

        ptrdiff_t *frame_offsets = NULL;
        if (framecount > 0)
        {
            frame_offsets = safe_malloc(framecount * sizeof(ptrdiff_t));
            for (int i = 0; i < framecount; i++)
            {
                frame_offsets[i] = (byte *)frames[i].pdata - lumpbuffer;
            }
        }

        buffer_size = needed * 2;
        lumpbuffer = safe_realloc(lumpbuffer, buffer_size);
        plump = lumpbuffer + current_offset;

        if (framecount > 0)
        {
            for (int i = 0; i < framecount; i++)
            {
                frames[i].pdata = lumpbuffer + frame_offsets[i];
            }
            free(frame_offsets);
        }
    }
}

static void ensure_frame_capacity(void)
{
    if (framecount >= max_frames)
    {
        max_frames *= 2;
        frames = safe_realloc(frames, max_frames * sizeof(spritepackage_t));
    }
}

static void load_bmp(const char *filename)
{
    const char *path_to_open = filename;
    char *fullpath = NULL;

    if (!is_absolute_path(filename))
    {
        size_t needed = strlen(spritedir) + strlen(filename) + 1;
        fullpath = safe_malloc(needed);
        strcpy(fullpath, spritedir);
        strcat(fullpath, filename);
        path_to_open = fullpath;
    }

    FILE *f = safe_open_read(path_to_open);

    byte header[54];
    safe_read(f, header, 54);

    if (header[0] != 'B' || header[1] != 'M')
    {
        error("%s is not a valid BMP file", path_to_open);
    }

    int width = *(int *)(header + 18);
    int height = *(int *)(header + 22);
    short bpp = *(short *)(header + 28);
    int data_offset = *(int *)(header + 10);
    int colors_used = *(int *)(header + 46);

    if (width <= 0 || height <= 0)
    {
        error("Invalid dimensions in %s", path_to_open);
    }

    byteimagewidth = width;
    byteimageheight = height;

    if (lbmpalette)
        free(lbmpalette);
    lbmpalette = safe_malloc(PALETTE_SIZE * 3);

    if (bpp == 8)
    {
        int palette_colors = colors_used ? colors_used : PALETTE_SIZE;
        fseek(f, 54, SEEK_SET);

        for (int i = 0; i < palette_colors; i++)
        {
            byte bgra[4];
            if (fread(bgra, 1, 4, f) != 4)
            {
                memset(bgra, 0, 4);
            }
            lbmpalette[i * 3] = bgra[2];
            lbmpalette[i * 3 + 1] = bgra[1];
            lbmpalette[i * 3 + 2] = bgra[0];
        }

        for (int i = palette_colors; i < PALETTE_SIZE; i++)
        {
            lbmpalette[i * 3] = 0;
            lbmpalette[i * 3 + 1] = 0;
            lbmpalette[i * 3 + 2] = 0;
        }

        if (!palette_established)
        {
            if (original_palette)
                free(original_palette);
            original_palette = safe_malloc(PALETTE_SIZE * 3);
            memcpy(original_palette, lbmpalette, PALETTE_SIZE * 3);
            palette_established = true;
        }
        else
        {
            memcpy(lbmpalette, original_palette, PALETTE_SIZE * 3);
        }
    }
    else
    {
        if (!palette_established)
        {
            fseek(f, data_offset, SEEK_SET);
            int row_size = ((width * bpp + 31) / 32) * 4;
            byte *row_buffer = safe_malloc(row_size);

            typedef struct
            {
                byte r, g, b;
            } color_t;
            color_t *unique_colors = safe_malloc(PALETTE_SIZE * sizeof(color_t));
            int palette_index = 0;

            for (int y = height - 1; y >= 0 && palette_index < 256; y--)
            {
                if (fread(row_buffer, 1, row_size, f) != (size_t)row_size)
                {
                    memset(row_buffer, 0, row_size);
                }

                for (int x = 0; x < width && palette_index < PALETTE_SIZE; x++)
                {
                    byte r, g, b;
                    if (bpp == 24)
                    {
                        b = row_buffer[x * 3];
                        g = row_buffer[x * 3 + 1];
                        r = row_buffer[x * 3 + 2];
                    }
                    else if (bpp == 32)
                    {
                        b = row_buffer[x * 4];
                        g = row_buffer[x * 4 + 1];
                        r = row_buffer[x * 4 + 2];
                    }
                    else
                    {
                        r = g = b = 0;
                    }

                    bool found = false;
                    for (int i = 0; i < palette_index; i++)
                    {
                        if (unique_colors[i].r == r && unique_colors[i].g == g && unique_colors[i].b == b)
                        {
                            found = true;
                            break;
                        }
                    }

                    if (!found)
                    {
                        unique_colors[palette_index].r = r;
                        unique_colors[palette_index].g = g;
                        unique_colors[palette_index].b = b;
                        lbmpalette[palette_index * 3] = r;
                        lbmpalette[palette_index * 3 + 1] = g;
                        lbmpalette[palette_index * 3 + 2] = b;
                        palette_index++;
                    }
                }
            }

            for (int i = palette_index; i < PALETTE_SIZE; i++)
            {
                lbmpalette[i * 3] = 0;
                lbmpalette[i * 3 + 1] = 0;
                lbmpalette[i * 3 + 2] = 0;
            }

            if (original_palette)
                free(original_palette);
            original_palette = safe_malloc(PALETTE_SIZE * 3);
            memcpy(original_palette, lbmpalette, PALETTE_SIZE * 3);
            palette_established = true;

            free(unique_colors);
            free(row_buffer);
        }
        else
        {
            memcpy(lbmpalette, original_palette, PALETTE_SIZE * 3);
        }
    }

    fseek(f, data_offset, SEEK_SET);

    int row_size = ((width * bpp + 31) / 32) * 4;
    byte *row_buffer = safe_malloc(row_size);

    if (byteimage)
        free(byteimage);
    byteimage = safe_malloc(width * height);

    for (int y = height - 1; y >= 0; y--)
    {
        if (fread(row_buffer, 1, row_size, f) != (size_t)row_size)
        {
            memset(row_buffer, 0, row_size);
        }

        for (int x = 0; x < width; x++)
        {
            if (bpp == 8)
            {
                byteimage[y * width + x] = row_buffer[x];
            }
            else if (bpp == 24)
            {
                byte b = row_buffer[x * 3];
                byte g = row_buffer[x * 3 + 1];
                byte r = row_buffer[x * 3 + 2];

                int best_match = 0;
                int best_distance = 999999;

                for (int i = 0; i < PALETTE_SIZE; i++)
                {
                    int dr = r - lbmpalette[i * 3];
                    int dg = g - lbmpalette[i * 3 + 1];
                    int db = b - lbmpalette[i * 3 + 2];
                    int distance = dr * dr + dg * dg + db * db;

                    if (distance < best_distance)
                    {
                        best_distance = distance;
                        best_match = i;
                    }
                }

                byteimage[y * width + x] = best_match;
            }
            else if (bpp == 32)
            {
                byte b = row_buffer[x * 4];
                byte g = row_buffer[x * 4 + 1];
                byte r = row_buffer[x * 4 + 2];

                int best_match = 0;
                int best_distance = 999999;

                for (int i = 0; i < PALETTE_SIZE; i++)
                {
                    int dr = r - lbmpalette[i * 3];
                    int dg = g - lbmpalette[i * 3 + 1];
                    int db = b - lbmpalette[i * 3 + 2];
                    int distance = dr * dr + dg * dg + db * db;

                    if (distance < best_distance)
                    {
                        best_distance = distance;
                        best_match = i;
                    }
                }

                byteimage[y * width + x] = best_match;
            }
        }
    }

    free(row_buffer);
    fclose(f);

    if (fullpath)
        free(fullpath);
}

static void grab_frame(void)
{
    dspriteframe_t *pframe;
    int x, y, xl, yl, w, h;
    byte *source;

    get_token(false);
    xl = atoi(token);
    get_token(false);
    yl = atoi(token);
    get_token(false);
    w = atoi(token);
    get_token(false);
    h = atoi(token);

    if (xl < 0 || yl < 0 || w <= 0 || h <= 0 ||
        xl + w > byteimagewidth || yl + h > byteimageheight)
    {
        error("Bad frame coordinates");
    }

    ensure_frame_capacity();

    frames[framecount].type = SPR_SINGLE;

    size_t frame_size = sizeof(dspriteframe_t) + w * h;
    ensure_buffer_capacity((plump - lumpbuffer) + frame_size);

    pframe = (dspriteframe_t *)plump;

    if (get_token(false))
    {
        frames[framecount].interval = atof(token);
        if (frames[framecount].interval <= 0.0)
            error("Non-positive interval");
    }
    else
    {
        frames[framecount].interval = 0.1f;
    }

    if (get_token(false))
    {
        pframe->origin[0] = -atoi(token);
        get_token(false);
        pframe->origin[1] = atoi(token);
    }
    else
    {
        pframe->origin[0] = -(w >> 1);
        pframe->origin[1] = h >> 1;
    }

    pframe->width = w;
    pframe->height = h;

    plump += sizeof(dspriteframe_t);

    if (w > framesmaxs[0])
        framesmaxs[0] = w;
    if (h > framesmaxs[1])
        framesmaxs[1] = h;

    source = byteimage + yl * byteimagewidth + xl;

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            *plump++ = source[x];
        }
        source += byteimagewidth;
    }

    frames[framecount].pdata = pframe;
    framecount++;
}

static void finish_sprite(void)
{
    FILE *spriteouthandle;
    int i, curframe;
    dsprite_t spritetemp;
    dspriteframetype_t frametype;

    if (framecount == 0)
    {
        error("No frames\n");
    }

    sprite.numframes = framecount;
    sprite.boundingradius = sqrt(((framesmaxs[0] >> 1) * (framesmaxs[0] >> 1)) +
                                 ((framesmaxs[1] >> 1) * (framesmaxs[1] >> 1)));
    sprite.width = framesmaxs[0];
    sprite.height = framesmaxs[1];

    if (!spriteoutname)
        error("No output file specified. Use $spritename in the script or provide -o/--output");

    spriteouthandle = safe_open_write(spriteoutname);

    spritetemp.ident = little_long(IDSPRITEHEADER);
    spritetemp.version = little_long(SPRITE_VERSION);
    spritetemp.type = little_long(sprite.type);
    spritetemp.texFormat = little_long(sprite.texFormat);
    spritetemp.boundingradius = little_float(sprite.boundingradius);
    spritetemp.width = little_long(framesmaxs[0]);
    spritetemp.height = little_long(framesmaxs[1]);
    spritetemp.numframes = little_long(sprite.numframes);
    spritetemp.beamlength = little_float(sprite.beamlength);
    spritetemp.synctype = little_long(sprite.synctype);

    safe_write(spriteouthandle, &spritetemp, sizeof(spritetemp));

    if (do16bit)
    {
        short cnt = PALETTE_SIZE;
        safe_write(spriteouthandle, &cnt, sizeof(cnt));
        safe_write(spriteouthandle, lbmpalette, cnt * 3);
    }

    curframe = 0;
    for (i = 0; i < sprite.numframes; i++)
    {
        frametype.type = little_long(frames[curframe].type);
        safe_write(spriteouthandle, &frametype, sizeof(frametype));

        if (frames[curframe].type == SPR_SINGLE)
        {
            dspriteframe_t *pframe = (dspriteframe_t *)frames[curframe].pdata;
            dspriteframe_t frametemp;

            frametemp.origin[0] = little_long(pframe->origin[0]);
            frametemp.origin[1] = little_long(pframe->origin[1]);
            frametemp.width = little_long(pframe->width);
            frametemp.height = little_long(pframe->height);

            safe_write(spriteouthandle, &frametemp, sizeof(frametemp));
            safe_write(spriteouthandle, (byte *)(pframe + 1), pframe->width * pframe->height);
            curframe++;
        }
        else
        {
            int j, numframes;
            dspritegroup_t dsgroup;
            float totinterval;
            int groupframe;

            groupframe = curframe;
            curframe++;
            numframes = frames[groupframe].numgroupframes;

            dsgroup.numframes = little_long(numframes);
            safe_write(spriteouthandle, &dsgroup, sizeof(dsgroup));

            totinterval = 0.0;
            for (j = 0; j < numframes; j++)
            {
                dspriteinterval_t temp;
                totinterval += frames[groupframe + 1 + j].interval;
                temp.interval = little_float(totinterval);
                safe_write(spriteouthandle, &temp, sizeof(temp));
            }

            for (j = 0; j < numframes; j++)
            {
                dspriteframe_t *pframe = (dspriteframe_t *)frames[curframe].pdata;
                dspriteframe_t frametemp;

                frametemp.origin[0] = little_long(pframe->origin[0]);
                frametemp.origin[1] = little_long(pframe->origin[1]);
                frametemp.width = little_long(pframe->width);
                frametemp.height = little_long(pframe->height);

                safe_write(spriteouthandle, &frametemp, sizeof(frametemp));
                safe_write(spriteouthandle, (byte *)(pframe + 1), pframe->width * pframe->height);
                curframe++;
            }
        }
    }

    fclose(spriteouthandle);

    printf("sprgen: successful\n");
    printf("%d frame(s)\n", framecount);
    printf("%d ungrouped frame(s), including group headers\n", sprite.numframes);
}

static void parse_script(void)
{
    while (get_token(true))
    {
        if (!strcmp(token, "$spritename"))
        {
            if (framecount > 0)
                finish_sprite();

            get_token(false);
            if (spriteoutname)
                free(spriteoutname);
            if (cli_output_name)
            {
                if (cli_output_consumed)
                    error("Multiple $spritename entries are not supported when an output file is specified");
                spriteoutname = safe_malloc(strlen(cli_output_name) + 1);
                strcpy(spriteoutname, cli_output_name);
                cli_output_consumed = true;
            }
            else
            {
                spriteoutname = safe_malloc(strlen(spritedir) + strlen(token) + 16);
                sprintf(spriteoutname, "%s%s.spr", spritedir, token);
            }

            memset(&sprite, 0, sizeof(sprite));
            framecount = 0;
            palette_established = false;
            framesmaxs[0] = -9999999;
            framesmaxs[1] = -9999999;
            sprite.synctype = ST_RAND;
            sprite.type = SPR_VP_PARALLEL_UPRIGHT;
            sprite.texFormat = SPR_NORMAL;
            sprite.beamlength = 0;
        }
        else if (!strcmp(token, "$type"))
        {
            get_token(false);
            if (!strcmp(token, "vp_parallel_upright"))
                sprite.type = SPR_VP_PARALLEL_UPRIGHT;
            else if (!strcmp(token, "facing_upright"))
                sprite.type = SPR_FACING_UPRIGHT;
            else if (!strcmp(token, "vp_parallel"))
                sprite.type = SPR_VP_PARALLEL;
            else if (!strcmp(token, "oriented"))
                sprite.type = SPR_ORIENTED;
            else if (!strcmp(token, "vp_parallel_oriented"))
                sprite.type = SPR_VP_PARALLEL_ORIENTED;
            else
                error("Bad type: %s", token);
        }
        else if (!strcmp(token, "$texture"))
        {
            get_token(false);
            if (!strcmp(token, "normal"))
                sprite.texFormat = SPR_NORMAL;
            else if (!strcmp(token, "additive"))
                sprite.texFormat = SPR_ADDITIVE;
            else if (!strcmp(token, "indexalpha"))
                sprite.texFormat = SPR_INDEXALPHA;
            else if (!strcmp(token, "alphatest"))
                sprite.texFormat = SPR_ALPHTEST;
            else
                error("Bad texture format: %s", token);
        }
        else if (!strcmp(token, "$beamlength"))
        {
            get_token(false);
            sprite.beamlength = atof(token);
        }
        else if (!strcmp(token, "$sync"))
        {
            sprite.synctype = ST_SYNC;
        }
        else if (!strcmp(token, "$load"))
        {
            get_token(false);
            load_bmp(token);
        }
        else if (!strcmp(token, "$frame"))
        {
            grab_frame();
            sprite.numframes++;
        }
        else if (!strcmp(token, "$groupstart"))
        {
            int groupframe = framecount++;
            frames[groupframe].type = SPR_GROUP;
            frames[groupframe].numgroupframes = 0;

            while (get_token(true))
            {
                if (!strcmp(token, "$frame"))
                {
                    grab_frame();
                    frames[groupframe].numgroupframes++;
                }
                else if (!strcmp(token, "$load"))
                {
                    get_token(false);
                    load_bmp(token);
                }
                else if (!strcmp(token, "$groupend"))
                {
                    break;
                }
                else
                {
                    error("$frame, $load, or $groupend expected");
                }
            }

            if (frames[groupframe].numgroupframes == 0)
                error("Empty group");

            sprite.numframes++;
        }
        else
        {
            error("Unknown token: %s", token);
        }
    }
}

int main(int argc, char **argv)
{
    int i;
    char *filename = NULL;

    printf("sprgen\n");

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-16bit"))
        {
            do16bit = true;
        }
        else if (!strcmp(argv[i], "-no16bit"))
        {
            do16bit = false;
        }
        else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output"))
        {
            if (i + 1 >= argc)
                error("Option %s requires a value", argv[i]);
            i++;
            if (cli_output_name)
                free(cli_output_name);
            cli_output_name = safe_malloc(strlen(argv[i]) + 1);
            strcpy(cli_output_name, argv[i]);
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-help"))
        {
            printf("Usage: %s [options] file.qc\n", argv[0]);
            printf("Options:\n");
            printf("  -16bit          Enable 16-bit mode (default)\n");
            printf("  -no16bit        Disable 16-bit mode\n");
            printf("  -o, --output    Override output sprite file path\n");
            printf("  --help          Show this help\n");
            return 0;
        }
        else if (argv[i][0] == '-')
        {
            error("Unknown option: %s", argv[i]);
        }
        else
        {
            filename = argv[i];
        }
    }

    if (!filename)
    {
        error("No input file specified");
    }

    lumpbuffer = safe_malloc(buffer_size);
    plump = lumpbuffer;
    frames = safe_malloc(max_frames * sizeof(spritepackage_t));

    spritedir = safe_malloc(MAX_PATH_SIZE);
    strcpy(spritedir, filename);
    int slash_index = -1;
    for (int j = (int)strlen(spritedir) - 1; j >= 0; j--)
    {
        if (spritedir[j] == '/' || spritedir[j] == '\\')
        {
            spritedir[j + 1] = 0;
            slash_index = j;
            break;
        }
    }
    if (slash_index < 0)
        strcpy(spritedir, "./");

    sprite.synctype = ST_RAND;
    sprite.type = SPR_VP_PARALLEL_UPRIGHT;
    sprite.texFormat = SPR_NORMAL;
    sprite.beamlength = 0;

    start_script_parse(filename);
    parse_script();
    end_script_parse();

    if (framecount > 0)
        finish_sprite();

    free(lumpbuffer);
    free(frames);
    free(spritedir);
    free(spriteoutname);
    free(byteimage);
    free(lbmpalette);
    free(original_palette);
    free(token);
    if (cli_output_name)
        free(cli_output_name);

    return 0;
}
