#include <stdio.h>
#include <stdbool.h>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>

#define CLAY_IMPLEMENTATION
#include "external/clay/clay.h"
#include "external/clay/clay_renderer_SDL3.c"

static const Uint32 FONT_ID = 0;

static const Clay_Color COLOR_BG = { 235, 235, 235, 255 };
static const Clay_Color COLOR_TOOLBAR = { 200, 200, 200, 255 };
static const Clay_Color COLOR_SURFACE = { 0, 0, 0, 255 };
static const Clay_Color COLOR_TEXT = { 30, 30, 30, 255 };
static const Clay_Color COLOR_SQUARE_BLACK = {100, 100, 100, 255};
static const Clay_Color COLOR_SQUARE_WHITE = {200, 200, 200, 255};


typedef enum {
    EMPTY = 0,
    WHITE_PAWN, WHITE_KNIGHT, WHITE_BISHOP, WHITE_ROOK, WHITE_QUEEN, WHITE_KING,
    BLACK_PAWN, BLACK_KNIGHT, BLACK_BISHOP, BLACK_ROOK, BLACK_QUEEN, BLACK_KING
} PieceType;

typedef struct {
    PieceType board[8][8];
    bool whiteToMove;
    int selectedRow;
    int selectedCol;
    bool hasCastledWhite[2];  // [kingside, queenside]
    bool hasCastledBlack[2];
    int enPassantCol;  // -1 if none available
    SDL_Texture* pieceTextures[13]; // index = PieceType
} ChessState;
typedef struct app_state {
    SDL_Window* window;
    Clay_SDL3RendererData rendererData;
    ChessState chess;
} AppState;

static inline Clay_Dimensions SDL_MeasureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) {
    TTF_Font** fonts = userData;
    TTF_Font* font = fonts[config->fontId];
    int width = 0;
    int height = 0;
    TTF_SetFontSize(font, config->fontSize);
    if (!TTF_GetStringSize(font, text.chars, text.length, &width, &height)) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to measure text: %s", SDL_GetError());
    }
    return (Clay_Dimensions){ (float)width, (float)height };
}

static void HandleClayErrors(Clay_ErrorData errorData) {
    if (errorData.errorText.chars) {
        printf("%s\n", errorData.errorText.chars);
    }
}


static SDL_Texture* LoadTexture(SDL_Renderer* renderer, const char* path) {
    SDL_Surface* surface = IMG_Load(path);
    if (!surface) {
        SDL_Log("IMG_Load failed: %s", path);
        return NULL;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);

    if (!tex) {
        SDL_Log("CreateTexture failed: %s", path);
    }

    return tex;
}
static void LoadChessTextures(ChessState* chess, SDL_Renderer* renderer) {
    memset(chess->pieceTextures, 0, sizeof(chess->pieceTextures));

    chess->pieceTextures[WHITE_PAWN]   = LoadTexture(renderer, "external/resources/chess_pieces/wp.png");
    chess->pieceTextures[WHITE_KNIGHT] = LoadTexture(renderer, "external/resources/chess_pieces/wn.png");
    chess->pieceTextures[WHITE_BISHOP] = LoadTexture(renderer, "external/resources/chess_pieces/wb.png");
    chess->pieceTextures[WHITE_ROOK]   = LoadTexture(renderer, "external/resources/chess_pieces/wr.png");
    chess->pieceTextures[WHITE_QUEEN]  = LoadTexture(renderer, "external/resources/chess_pieces/wq.png");
    chess->pieceTextures[WHITE_KING]   = LoadTexture(renderer, "external/resources/chess_pieces/wk.png");

    chess->pieceTextures[BLACK_PAWN]   = LoadTexture(renderer, "external/resources/chess_pieces/bp.png");
    chess->pieceTextures[BLACK_KNIGHT] = LoadTexture(renderer, "external/resources/chess_pieces/bn.png");
    chess->pieceTextures[BLACK_BISHOP] = LoadTexture(renderer, "external/resources/chess_pieces/bb.png");
    chess->pieceTextures[BLACK_ROOK]   = LoadTexture(renderer, "external/resources/chess_pieces/br.png");
    chess->pieceTextures[BLACK_QUEEN]  = LoadTexture(renderer, "external/resources/chess_pieces/bq.png");
    chess->pieceTextures[BLACK_KING]   = LoadTexture(renderer, "external/resources/chess_pieces/bk.png");
}


ChessState initChessState() {
    ChessState chess = {0};
    chess.board[0][0] = WHITE_ROOK;
    chess.board[0][1] = WHITE_KNIGHT;
    chess.board[0][2] = WHITE_BISHOP;
    chess.board[0][3] = WHITE_QUEEN;
    chess.board[0][4] = WHITE_KING;
    chess.board[0][5] = WHITE_BISHOP;
    chess.board[0][6] = WHITE_KNIGHT;
    chess.board[0][7] = WHITE_ROOK;
    chess.board[7][0] = BLACK_ROOK;
    chess.board[7][1] = BLACK_KNIGHT;
    chess.board[7][2] = BLACK_BISHOP;
    chess.board[7][3] = BLACK_QUEEN;
    chess.board[7][4] = BLACK_KING;
    chess.board[7][5] = BLACK_BISHOP;
    chess.board[7][6] = BLACK_KNIGHT;
    chess.board[7][7] = BLACK_ROOK;
    for (int i = 0; i < 8; i++) {
        chess.board[1][i] = WHITE_PAWN;
        chess.board[6][i] = BLACK_PAWN;
    }
    for (int row = 2; row < 6; row++) {
        for (int col = 0; col < 8; col++) {
            chess.board[row][col] = EMPTY;
        }
    }
    chess.whiteToMove = true;
    chess.selectedRow = -1;
    chess.selectedCol = -1;
    chess.hasCastledWhite[0] = false;
    chess.hasCastledWhite[1] = false;
    chess.hasCastledBlack[0] = false;
    chess.hasCastledBlack[1] = false;
    chess.enPassantCol = -1;
    return chess;
}

void renderChessPiece(SDL_Texture** pieceTextures, PieceType piece, Clay_String squareIdString)
{
    if (piece == EMPTY) return;
    SDL_Texture* tex = pieceTextures[piece];
    if (!tex) return;
    /* build ID: "<piece><square>" e.g. "wp_A1" */
    char idBuf[16];
    char pieceChar = (char)(piece - '0');
    /* Example result: "P_A1" */
    int len = SDL_snprintf(
        idBuf,
        sizeof(idBuf),
        "%c_%.*s",
        pieceChar,
        (int)squareIdString.length,
        squareIdString.chars
    );
    Clay_String pieceId = {
        .chars  = idBuf,
        .length = len
    };
    CLAY(CLAY_SID(pieceId), { .aspectRatio = 1, .layout = {.sizing={.width = CLAY_SIZING_GROW(60)}},.image = {.imageData = tex}}) {}
}

void renderChessBoard(ChessState chess,bool isWhite){
    static char rowNames[8][2];
    static bool initialized=false;
    if(!initialized){for(char i=0;i<8;i++){rowNames[i][0]=(char)('1'+i);rowNames[i][1]='\0';}initialized=true;}
    Clay_Sizing expand={.width=CLAY_SIZING_GROW(0),.height=CLAY_SIZING_GROW(0)};
    Clay_String boardId={.chars="Board",.length=5};
    CLAY(CLAY_SID(boardId),{.aspectRatio=1,.layout={.sizing={.width=CLAY_SIZING_GROW(60*8)},.layoutDirection=CLAY_TOP_TO_BOTTOM,.padding=CLAY_PADDING_ALL(24)}}){
        for(char r=0;r<8;r++){
            char row = isWhite ? (char)(7 - r) : r;
            char rowStr[2]={(char)('1'+row),'\0'};
            Clay_String rowIdString={.chars=rowStr,.length=1};
            CLAY(CLAY_SID(rowIdString),{.aspectRatio=1.0f/8.0f,.layout={.sizing={.width=CLAY_SIZING_GROW(60*8), .height=CLAY_SIZING_GROW(60)}}}){ // row
                for(char c=0;c<8;c++){
                    char col = isWhite ? (char)(7 - c) : c;
                    char squareStr[3]={(char)('A'+col),(char)('1'+row),'\0'};
                    Clay_String squareIdString={.chars=squareStr,.length=2};
                    bool light = (((int)col + (int)row) % 2) == 0;
                    bool hovered = Clay_PointerOver(CLAY_SID(squareIdString));
                    Clay_Color squareColor = hovered ? (Clay_Color){255,0,0,255} : (light ? COLOR_SQUARE_WHITE : COLOR_SQUARE_BLACK);
                    CLAY(CLAY_SID(squareIdString),{.aspectRatio=1,.backgroundColor=squareColor,.layout={.sizing=expand}}){ renderChessPiece(chess.pieceTextures,chess.board[row][col],squareIdString); };
                }
            };
        }
    };
}




static Clay_RenderCommandArray CreateLayout(AppState* state) {
    Clay_BeginLayout();
    Clay_Sizing expand = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) };
    CLAY(CLAY_ID("Root"), { .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = expand }, .backgroundColor = COLOR_BG }) {
        CLAY(CLAY_ID("Toolbar"), {.layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(48) }, .padding = CLAY_PADDING_ALL(12) }, .backgroundColor = COLOR_TOOLBAR}) {
            CLAY_TEXT(CLAY_STRING("Toolbar"), CLAY_TEXT_CONFIG({ .fontId = FONT_ID, .fontSize = 20, .textColor = COLOR_TEXT }));
        }
        CLAY(CLAY_ID("Content"), { .layout = { .sizing = expand, .padding = CLAY_PADDING_ALL(24), .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
            renderChessBoard(state->chess, true);
        }
    }
    return Clay_EndLayout();
}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    if (!TTF_Init()) {
        return SDL_APP_FAILURE;
    }
    AppState* state = SDL_calloc(1, sizeof(AppState));
    if (!state) return SDL_APP_FAILURE;
    if (!SDL_CreateWindowAndRenderer("SDL + Clay UI", 900, 600, 0, &state->window, &state->rendererData.renderer)) {
        return SDL_APP_FAILURE;
    }
    SDL_SetWindowResizable(state->window, true);
    state->rendererData.textEngine = TTF_CreateRendererTextEngine(state->rendererData.renderer);
    state->rendererData.fonts = SDL_calloc(1, sizeof(TTF_Font*));
    TTF_Font* font = TTF_OpenFont("external/resources/Roboto-Regular.ttf", 24);
    if (!font) return SDL_APP_FAILURE;
    state->rendererData.fonts[FONT_ID] = font;
    uint64_t memSize = Clay_MinMemorySize();
    Clay_Arena arena = { .memory = SDL_malloc(memSize), .capacity = memSize };
    int w, h;
    SDL_GetWindowSize(state->window, &w, &h);
    Clay_Initialize(arena, (Clay_Dimensions){ (float)w, (float)h }, (Clay_ErrorHandler){ HandleClayErrors });
    Clay_SetDebugModeEnabled(true);
    Clay_SetMeasureTextFunction(SDL_MeasureText, state->rendererData.fonts);
    state->chess = initChessState();
    LoadChessTextures(&state->chess, state->rendererData.renderer);

    printf("SDL3+Clay Loaded\n");
    *appstate = state;
    return SDL_APP_CONTINUE;
}
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    (void)appstate;
    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;
        case SDL_EVENT_WINDOW_RESIZED:
            Clay_SetLayoutDimensions((Clay_Dimensions){ (float)event->window.data1, (float)event->window.data2 });
            break;
        case SDL_EVENT_MOUSE_MOTION:
            Clay_SetPointerState((Clay_Vector2){ event->motion.x, event->motion.y }, event->motion.state & SDL_BUTTON_LMASK);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            Clay_SetPointerState((Clay_Vector2){ event->button.x, event->button.y }, event->button.button == SDL_BUTTON_LEFT);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            Clay_UpdateScrollContainers(true, (Clay_Vector2){ event->wheel.x, event->wheel.y }, 0.01f);
            break;
        default:
            break;
    }
    return SDL_APP_CONTINUE;
}
SDL_AppResult SDL_AppIterate(void* appstate) {
    AppState* state = appstate;
    Clay_RenderCommandArray commands = CreateLayout(state);
    SDL_SetRenderDrawColor(state->rendererData.renderer, 20, 20, 20, 255);
    SDL_RenderClear(state->rendererData.renderer);
    SDL_Clay_RenderClayCommands(&state->rendererData, &commands);
    SDL_RenderPresent(state->rendererData.renderer);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    (void)result;
    AppState* state = appstate;
    if (!state) return;
    TTF_CloseFont(state->rendererData.fonts[FONT_ID]);
    SDL_free(state->rendererData.fonts);
    TTF_DestroyRendererTextEngine(state->rendererData.textEngine);
    SDL_DestroyRenderer(state->rendererData.renderer);
    SDL_DestroyWindow(state->window);
    SDL_free(state);
    TTF_Quit();
}