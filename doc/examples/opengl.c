//  This code is public domain.

#include "SDL2/SDL.h"
#include "SDL2/SDL_opengl.h"
#include "SDL2/SDL_video.h"

static GLfloat color[8][3] = {
    { 1.0, 1.0, 0.0 },
    { 1.0, 0.0, 0.0 },
    { 0.0, 0.0, 0.0 },
    { 0.0, 1.0, 0.0 },
    { 0.0, 1.0, 1.0 },
    { 1.0, 1.0, 1.0 },
    { 1.0, 0.0, 1.0 },
    { 0.0, 0.0, 1.0 },
};

static GLfloat cube[8][3] = {
    {  0.5,  0.5, -0.5 },
    {  0.5, -0.5, -0.5 },
    { -0.5, -0.5, -0.5 },
    { -0.5,  0.5, -0.5 },
    { -0.5,  0.5,  0.5 },
    {  0.5,  0.5,  0.5 },
    {  0.5, -0.5,  0.5 },
    { -0.5, -0.5,  0.5 },
};

/*     4_ _ _ 5
 *    /|     /|
 *  3/_|_ _0/ |
 *  |  |_ _|_ |
 *  | /7   | /6
 *  |/_ _ _|/
 *  2      1
 */

static GLubyte indices[] = {
    0, 1, 2,
    2, 3, 0,
    1, 2, 6,
    2, 6, 7,
    0, 3, 5,
    3, 4, 5,
    4, 7, 6,
    6, 5, 4,
    0, 1, 5,
    1, 6, 5,
    3, 2, 7,
    3, 4, 7,
};

#if 0
function pixel(x, y, color: LongInt): LongInt;
begin
    pixel:= RGB_Buffer[(cScreenHeight-y-1)*cScreenWidth*4 + x*4 + color];
end;

{
    numPixels:= cScreenWidth*cScreenHeight;
        YCbCr_Planes[0]:= GetMem(numPixels);
    YCbCr_Planes[1]:= GetMem(numPixels div 4);
    YCbCr_Planes[2]:= GetMem(numPixels div 4);
    RGB_Buffer:= GetMem(4*numPixels);

    // read pixels from OpenGL
    glReadPixels(0, 0, cScreenWidth, cScreenHeight, GL_RGBA, GL_UNSIGNED_BYTE, RGB_Buffer);

    // convert to YCbCr 4:2:0 format
    // Y
    for y := 0 to cScreenHeight-1 do
        for x := 0 to cScreenWidth-1 do
            YCbCr_Planes[0][y*cScreenWidth + x]:= Byte(16 + ((16828*pixel(x,y,0) + 33038*pixel(x,y,1) + 6416*pixel(x,y,2)) shr 16));

    // Cb and Cr
    for y := 0 to cScreenHeight div 2 - 1 do
        for x := 0 to cScreenWidth div 2 - 1 do
        begin
            r:= pixel(2*x,2*y,0) + pixel(2*x+1,2*y,0) + pixel(2*x,2*y+1,0) + pixel(2*x+1,2*y+1,0);
            g:= pixel(2*x,2*y,1) + pixel(2*x+1,2*y,1) + pixel(2*x,2*y+1,1) + pixel(2*x+1,2*y+1,1);
            b:= pixel(2*x,2*y,2) + pixel(2*x+1,2*y,2) + pixel(2*x,2*y+1,2) + pixel(2*x+1,2*y+1,2);
            YCbCr_Planes[1][y*(cScreenWidth div 2) + x]:= Byte(128 + ((-2428*r - 4768*g + 7196*b) shr 16));
            YCbCr_Planes[2][y*(cScreenWidth div 2) + x]:= Byte(128 + (( 7196*r - 6026*g - 1170*b) shr 16));
        end;

    if AVWrapper_WriteFrame(YCbCr_Planes[0], YCbCr_Planes[1], YCbCr_Planes[2]) < 0 then
        halt(-1);
        }
#endif
int main(void)
{
    SDL_Window *w;

    SDL_Init(SDL_INIT_VIDEO);
    w = SDL_CreateWindow("cube", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_CreateContext(w);
    SDL_GL_SetSwapInterval(1);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, cube);
    glColorPointer(3, GL_FLOAT, 0, color);
    glMatrixMode(GL_MODELVIEW);

    int done = 0;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                done = 1;
            }
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawElements(GL_TRIANGLES, sizeof(indices), GL_UNSIGNED_BYTE, indices);
        glRotatef(5.0, 1.0, 1.0, 1.0);
        SDL_GL_SwapWindow(w);
        //SDL_Delay(1000);
    }

    SDL_Quit();
    return 0;
}
