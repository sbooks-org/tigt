#include <tigt.h>
#include <tigt_video.h>

int main(void)
{
    if (tigt_cp437_codepoint(0x82) != 0x00e9)
        return 1;
    if (tigt_snapshot_write_fd(-1, TIGT_SNAPSHOT_PNG) != TIGT_ERROR_ARGUMENT)
        return 2;
    tigt_video *video = tigt_video_create(TIGT_VIDEO_CGA);
    if (video == NULL)
        return 3;
    uint8_t vram[16384] = {0};
    vram[2] = 0x82;
    vram[3] = 0x9e;
    tigt_video_write(video, 0x3d4, 1);
    tigt_video_write(video, 0x3d5, 80);
    tigt_video_write(video, 0x3d4, 13);
    tigt_video_write(video, 0x3d5, 1);
    tigt_video_write(video, 0x3d8, 0x09);
    tigt_video_frame frame;
    int valid = tigt_video_decode(video, vram, sizeof(vram), 0, &frame) == TIGT_OK &&
                frame.kind == TIGT_VIDEO_TEXT && frame.width == 80 && frame.height == 25 &&
                frame.cells[0].codepoint == 0x00e9 &&
                frame.cells[0].foreground == 0xffff55 && frame.cells[0].background == 0x5555ff;
    tigt_video_destroy(video);
    return valid ? 0 : 4;
}
