#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define IDSPRITEHEADER (('P' << 24) + ('S' << 16) + ('D' << 8) + 'I')

typedef struct
{
    int32_t ident;
    int32_t version;
    int32_t type;
    int32_t texFormat;
    float boundingradius;
    int32_t width;
    int32_t height;
    int32_t numframes;
    float beamlength;
    int32_t synctype;
} dsprite_t;

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <sprite.spr>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        printf("Error: Cannot open %s\n", argv[1]);
        return 1;
    }

    dsprite_t header;
    if (fread(&header, sizeof(header), 1, f) != 1)
    {
        printf("Error: Cannot read sprite header\n");
        fclose(f);
        return 1;
    }

    printf("Sprite Information for: %s\n", argv[1]);
    printf("================================\n");
    printf("Magic: 0x%08X (%s)\n", header.ident,
           header.ident == IDSPRITEHEADER ? "Valid" : "INVALID");
    printf("Version: %d\n", header.version);
    printf("Type: %d ", header.type);
    switch (header.type)
    {
    case 0:
        printf("(vp_parallel_upright)\n");
        break;
    case 1:
        printf("(facing_upright)\n");
        break;
    case 2:
        printf("(vp_parallel)\n");
        break;
    case 3:
        printf("(oriented)\n");
        break;
    case 4:
        printf("(vp_parallel_oriented)\n");
        break;
    default:
        printf("(unknown)\n");
        break;
    }
    printf("Texture Format: %d ", header.texFormat);
    switch (header.texFormat)
    {
    case 0:
        printf("(normal)\n");
        break;
    case 1:
        printf("(additive)\n");
        break;
    case 2:
        printf("(indexalpha)\n");
        break;
    case 3:
        printf("(alphachannel)\n");
        break;
    default:
        printf("(unknown)\n");
        break;
    }
    printf("Bounding Radius: %.2f\n", header.boundingradius);
    printf("Dimensions: %dx%d\n", header.width, header.height);
    printf("Frame Count: %d\n", header.numframes);
    printf("Beam Length: %.2f\n", header.beamlength);
    printf("Sync Type: %d (%s)\n", header.synctype,
           header.synctype == 0 ? "synchronized" : "random");

    if (header.texFormat == 0)
    {
        int16_t palette_size;
        if (fread(&palette_size, sizeof(palette_size), 1, f) == 1)
        {
            printf("Palette Size: %d colors\n", palette_size);
        }
    }

    fclose(f);
    return 0;
}
