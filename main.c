/* threaded SDL3 + Clay chess (SDL3 names fixed)
   - Fixes: SDL_Mutex type and SDL_DestroySurface usage
   - Engine runs in background thread; main thread applies move when ready.
*/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3/SDL_atomic.h>

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

int PIECE_VALUES[13] = { 0,
    1, 3, 4, 5, 9, 0,
    1, 3, 4, 5, 9, 0
};


typedef struct {
    int fromRow, fromCol;
    int toRow, toCol;
    PieceType captured;
    bool isPromotion;
    bool isEnPassant;
    bool isCastling;
} Move;

char* move2chars(Move move) {
    static char moveStr[32];
    char fromFile = 'a' + move.fromCol;
    char fromRank = '1' + move.fromRow;
    char toFile = 'a' + move.toCol;
    char toRank = '1' + move.toRow;
    if (move.isCastling) {
        if (move.toCol == 6) SDL_snprintf(moveStr, sizeof(moveStr), "O-O");
        else SDL_snprintf(moveStr, sizeof(moveStr), "O-O-O");
    } else if (move.isPromotion) {
        SDL_snprintf(moveStr, sizeof(moveStr), "%c%c-%c%c=Q", fromFile, fromRank, toFile, toRank);
    } else if (move.isEnPassant) {
        SDL_snprintf(moveStr, sizeof(moveStr), "%c%c x %c%c e.p.", fromFile, fromRank, toFile, toRank);
    } else if (move.captured != EMPTY) {
        SDL_snprintf(moveStr, sizeof(moveStr), "%c%c x %c%c", fromFile, fromRank, toFile, toRank);
    } else {
        SDL_snprintf(moveStr, sizeof(moveStr), "%c%c-%c%c", fromFile, fromRank, toFile, toRank);
    }
    return moveStr;
}

typedef struct {
    Move moves[256];
    int count;
} MoveList;

typedef struct {
    PieceType board[8][8];
    bool whiteToMove;
    int selectedRow;
    int selectedCol;
    Move engineMove;
    bool engineWhite;
    bool enginePending;
    bool hasCastledWhite[2];
    bool hasCastledBlack[2];
    int enPassantCol;
    SDL_Texture* pieceTextures[13];
} ChessState;

typedef struct {
    SDL_AtomicInt nodesSearched;
    SDL_AtomicInt depthCompleted;
    SDL_AtomicInt searching;   /* 1 = running, 0 = idle */
} EngineProgress;

typedef struct {
    EngineProgress progress;
    SDL_Thread* thread;
    SDL_Mutex* mutex;        /* SDL3 type (fixed) */
    bool hasMove;
    Move resultMove;
} Engine;

typedef struct {
    SDL_Window* window;
    Clay_SDL3RendererData rendererData;
    ChessState chess;
    Engine engine;
} AppState;

/* forward declarations */
void getAllMoves(ChessState* chess, MoveList* moves);
int evaluateMaterial(ChessState* chess, bool whitePerspective);
Move findBestMove(ChessState* chess, int depth, Engine* engine);
void makeMove(ChessState* chess, Move move, void* _undo);
void unmakeMove(ChessState* chess, Move move, void* _undo);

static inline Clay_Dimensions SDL_MeasureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) {
    TTF_Font** fonts = userData;
    TTF_Font* font = fonts[config->fontId];
    int width = 0, height = 0;
    TTF_SetFontSize(font, config->fontSize);
    if (!TTF_GetStringSize(font, text.chars, text.length, &width, &height)) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "TTF_GetStringSize failed: %s", SDL_GetError());
    }
    return (Clay_Dimensions){ (float)width, (float)height };
}

static void HandleClayErrors(Clay_ErrorData errorData) {
    if (errorData.errorText.chars) {
        printf("%.*s\n", (int)errorData.errorText.length, errorData.errorText.chars);
    }
}

static SDL_Texture* LoadTexture(SDL_Renderer* renderer, const char* path) {
    SDL_Surface* surf = IMG_Load(path);
    if (!surf) {
        SDL_Log("IMG_Load failed for %s: %s", path, SDL_GetError());
        return NULL;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf); /* SDL3 function (fixed) */
    if (!tex) SDL_Log("CreateTexture failed for %s: %s", path, SDL_GetError());
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

ChessState initChessState(void) {
    ChessState chess;
    memset(&chess, 0, sizeof(chess));
    chess.board[0][0] = WHITE_ROOK;   chess.board[0][1] = WHITE_KNIGHT; chess.board[0][2] = WHITE_BISHOP; chess.board[0][3] = WHITE_QUEEN;
    chess.board[0][4] = WHITE_KING;   chess.board[0][5] = WHITE_BISHOP; chess.board[0][6] = WHITE_KNIGHT; chess.board[0][7] = WHITE_ROOK;
    chess.board[7][0] = BLACK_ROOK;   chess.board[7][1] = BLACK_KNIGHT; chess.board[7][2] = BLACK_BISHOP; chess.board[7][3] = BLACK_QUEEN;
    chess.board[7][4] = BLACK_KING;   chess.board[7][5] = BLACK_BISHOP; chess.board[7][6] = BLACK_KNIGHT; chess.board[7][7] = BLACK_ROOK;
    for (int i = 0; i < 8; ++i) { chess.board[1][i] = WHITE_PAWN; chess.board[6][i] = BLACK_PAWN; }
    chess.whiteToMove = true;
    chess.selectedRow = -1; chess.selectedCol = -1;
    chess.enginePending = false;
    chess.enPassantCol = -1;
    chess.engineWhite = false;
    return chess;
}

/* helpers & move generation (kept similar to your logic) */
static inline bool inBounds(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }
static inline bool isWhite(PieceType p) { return p >= WHITE_PAWN && p <= WHITE_KING; }
static inline bool isBlack(PieceType p) { return p >= BLACK_PAWN && p <= BLACK_KING; }
static inline bool sameColor(PieceType a, PieceType b) { return (isWhite(a) && isWhite(b)) || (isBlack(a) && isBlack(b)); }

bool pathClear(const ChessState* chess, int fr, int fc, int tr, int tc) {
    int dr = (tr > fr) - (tr < fr);
    int dc = (tc > fc) - (tc < fc);
    for (int r = fr + dr, c = fc + dc; r != tr || c != tc; r += dr, c += dc) {
        if (chess->board[r][c] != EMPTY) return false;
    }
    return true;
}
bool knightMove(const ChessState* chess, int fr, int fc, int tr, int tc) {
    int dr = abs(tr - fr), dc = abs(tc - fc);
    return (dr == 2 && dc == 1) || (dr == 1 && dc == 2);
}
bool rookMove(const ChessState* chess, int fr, int fc, int tr, int tc) {
    if (fr != tr && fc != tc) return false;
    return pathClear(chess, fr, fc, tr, tc);
}
bool bishopMove(const ChessState* chess, int fr, int fc, int tr, int tc) {
    if (abs(fr - tr) != abs(fc - tc)) return false;
    return pathClear(chess, fr, fc, tr, tc);
}
bool queenMove(const ChessState* chess, int fr, int fc, int tr, int tc) {
    return rookMove(chess, fr, fc, tr, tc) || bishopMove(chess, fr, fc, tr, tc);
}
bool pawnMove(const ChessState* chess, int fr, int fc, int tr, int tc) {
    PieceType p = chess->board[fr][fc];
    int dir = isWhite(p) ? +1 : -1;
    int startRow = isWhite(p) ? 1 : 6;
    if (fc == tc && tr == fr + dir && chess->board[tr][tc] == EMPTY) return true;
    if (fc == tc && fr == startRow && tr == fr + 2*dir && chess->board[fr + dir][fc] == EMPTY && chess->board[tr][tc] == EMPTY) return true;
    if (abs(tc - fc) == 1 && tr == fr + dir && chess->board[tr][tc] != EMPTY) return true;
    return false;
}
bool canPieceAttackSquare(const ChessState* chess, int fr, int fc, int tr, int tc) {
    if (!inBounds(tr, tc)) return false;
    PieceType p = chess->board[fr][fc];
    if (p == EMPTY) return false;
    switch (p) {
        case WHITE_PAWN: case BLACK_PAWN: return pawnMove(chess, fr, fc, tr, tc);
        case WHITE_KNIGHT: case BLACK_KNIGHT: return knightMove(chess, fr, fc, tr, tc);
        case WHITE_BISHOP: case BLACK_BISHOP: return bishopMove(chess, fr, fc, tr, tc);
        case WHITE_ROOK: case BLACK_ROOK: return rookMove(chess, fr, fc, tr, tc);
        case WHITE_QUEEN: case BLACK_QUEEN: return queenMove(chess, fr, fc, tr, tc);
        case WHITE_KING: case BLACK_KING: return (abs(fr - tr) <= 1 && abs(fc - tc) <= 1);
        default: return false;
    }
}
bool isSquareAttacked(const ChessState* chess, int r, int c, bool byWhite) {
    for (int fr = 0; fr < 8; fr++) for (int fc = 0; fc < 8; fc++) {
        PieceType p = chess->board[fr][fc];
        if (p == EMPTY) continue;
        if (byWhite && !isWhite(p)) continue;
        if (!byWhite && !isBlack(p)) continue;
        if (canPieceAttackSquare(chess, fr, fc, r, c)) return true;
    }
    return false;
}
bool isKingInCheck(const ChessState* chess, bool whiteKing) {
    for (int r=0;r<8;r++) for (int c=0;c<8;c++){
        PieceType p = chess->board[r][c];
        if ((whiteKing && p == WHITE_KING) || (!whiteKing && p == BLACK_KING)) {
            return isSquareAttacked(chess, r, c, !whiteKing);
        }
    }
    return false;
}
bool canCastle(const ChessState* chess, int fr, int fc, int tr, int tc) {
    PieceType king = chess->board[fr][fc];
    bool white = isWhite(king);
    if (white && fr != 0) return false;
    if (!white && fr != 7) return false;
    if (fc != 4) return false;
    int rookCol = (tc == 6) ? 7 : 0;
    int step = (tc > fc) ? 1 : -1;
    if (white && chess->hasCastledWhite[(tc == 6) ? 0 : 1]) return false;
    if (!white && chess->hasCastledBlack[(tc == 6) ? 0 : 1]) return false;
    if (chess->board[fr][rookCol] != (white ? WHITE_ROOK : BLACK_ROOK)) return false;
    for (int c = fc + step; c != rookCol; c += step) if (chess->board[fr][c] != EMPTY) return false;
    if (isKingInCheck(chess, white)) return false;
    for (int c = fc; c != tc + step; c += step) if (isSquareAttacked(chess, fr, c, !white)) return false;
    return true;
}
bool kingMove(const ChessState* chess, int fr, int fc, int tr, int tc) {
    if (abs(fr - tr) <= 1 && abs(fc - tc) <= 1) return true;
    if (fr == tr && (tc == 6 || tc == 2)) return canCastle(chess, fr, fc, tr, tc);
    return false;
}
bool isLegalMove(const ChessState* chess, int fr, int fc, int tr, int tc) {
    if (!inBounds(tr, tc)) return false;
    PieceType p = chess->board[fr][fc];
    if (p == EMPTY) return false;
    PieceType target = chess->board[tr][tc];
    if (sameColor(p, target)) return false;
    bool ok = false;
    switch (p) {
        case WHITE_PAWN: case BLACK_PAWN: ok = pawnMove(chess, fr, fc, tr, tc); break;
        case WHITE_KNIGHT: case BLACK_KNIGHT: ok = knightMove(chess, fr, fc, tr, tc); break;
        case WHITE_BISHOP: case BLACK_BISHOP: ok = bishopMove(chess, fr, fc, tr, tc); break;
        case WHITE_ROOK: case BLACK_ROOK: ok = rookMove(chess, fr, fc, tr, tc); break;
        case WHITE_QUEEN: case BLACK_QUEEN: ok = queenMove(chess, fr, fc, tr, tc); break;
        case WHITE_KING: case BLACK_KING: ok = kingMove(chess, fr, fc, tr, tc); break;
        default: return false;
    }
    if (!ok) return false;
    ChessState copy = *chess;
    copy.board[tr][tc] = copy.board[fr][fc];
    copy.board[fr][fc] = EMPTY;
    bool white = isWhite(copy.board[tr][tc]);
    return !isKingInCheck(&copy, white);
}

void getAllMoves(ChessState* chess, MoveList* moves) {
    moves->count = 0;
    for (int r=0;r<8;r++){
        for (int c=0;c<8;c++){
            PieceType p = chess->board[r][c];
            if (p == EMPTY) continue;
            if (chess->whiteToMove && isBlack(p)) continue;
            if (!chess->whiteToMove && isWhite(p)) continue;
            for (int r2=0;r2<8;r2++){
                for (int c2=0;c2<8;c2++){
                    if (isLegalMove(chess, r, c, r2, c2)) {
                        if (moves->count >= 256) continue;
                        Move mv = { r, c, r2, c2, chess->board[r2][c2], false, false, false };
                        if ((p == WHITE_KING || p == BLACK_KING) && c == 4 && abs(c2 - c) == 2) mv.isCastling = true;
                        if ((p == WHITE_PAWN && r2 == 7) || (p == BLACK_PAWN && r2 == 0)) mv.isPromotion = true;
                        if ((p == WHITE_PAWN || p == BLACK_PAWN) && c != c2 && chess->board[r2][c2] == EMPTY) {
                            mv.isEnPassant = true;
                            int capRow = isWhite(p) ? r2 - 1 : r2 + 1;
                            mv.captured = chess->board[capRow][c2];
                        }
                        moves->moves[moves->count++] = mv;
                    }
                }
            }
        }
    }
}

int evaluateMaterial(ChessState* chess, bool whitePerspective) {
    int value = 0;
    for (int r=0;r<8;r++) for (int c=0;c<8;c++){
        PieceType p = chess->board[r][c];
        if (p == EMPTY) continue;
        int val = PIECE_VALUES[p];
        if (isWhite(p)) value += (whitePerspective ? val : -val);
        else value += (whitePerspective ? -val : val);
    }
    return value;
}

typedef struct {
    bool hasCastledWhite[2];
    bool hasCastledBlack[2];
    int enPassantCol;
    PieceType capturedPiece;
    int capturedRow;
    int capturedCol;
} UndoInfo;

void makeMove(ChessState* chess, Move move, void* _undo) {
    UndoInfo undoLocal;
    UndoInfo* undo = (UndoInfo*)_undo;
    if (!undo) undo = &undoLocal;

    undo->hasCastledWhite[0] = chess->hasCastledWhite[0];
    undo->hasCastledWhite[1] = chess->hasCastledWhite[1];
    undo->hasCastledBlack[0] = chess->hasCastledBlack[0];
    undo->hasCastledBlack[1] = chess->hasCastledBlack[1];
    undo->enPassantCol = chess->enPassantCol;
    undo->capturedPiece = move.captured;

    PieceType moving = chess->board[move.fromRow][move.fromCol];

    if (move.isCastling) {
        int rookFromCol = (move.toCol == 6) ? 7 : 0;
        int rookToCol   = (move.toCol == 6) ? 5 : 3;
        chess->board[move.fromRow][rookToCol] = chess->board[move.fromRow][rookFromCol];
        chess->board[move.fromRow][rookFromCol] = EMPTY;

        if (moving == WHITE_KING) chess->hasCastledWhite[(move.toCol == 6) ? 0 : 1] = true;
        else chess->hasCastledBlack[(move.toCol == 6) ? 0 : 1] = true;
    }

    if (move.isEnPassant) {
        int capturedRow = isWhite(moving) ? move.toRow - 1 : move.toRow + 1;
        undo->capturedRow = capturedRow;
        undo->capturedCol = move.toCol;
        chess->board[capturedRow][move.toCol] = EMPTY;
    }

    if (move.isPromotion) moving = isWhite(moving) ? WHITE_QUEEN : BLACK_QUEEN;

    chess->enPassantCol = -1;
    if ((moving == WHITE_PAWN || moving == BLACK_PAWN) && abs(move.toRow - move.fromRow) == 2) {
        chess->enPassantCol = move.fromCol;
    }

    if (moving == WHITE_KING) { chess->hasCastledWhite[0] = true; chess->hasCastledWhite[1] = true; }
    else if (moving == BLACK_KING) { chess->hasCastledBlack[0] = true; chess->hasCastledBlack[1] = true; }
    else if (moving == WHITE_ROOK) {
        if (move.fromRow == 0 && move.fromCol == 7) chess->hasCastledWhite[0] = true;
        if (move.fromRow == 0 && move.fromCol == 0) chess->hasCastledWhite[1] = true;
    } else if (moving == BLACK_ROOK) {
        if (move.fromRow == 7 && move.fromCol == 7) chess->hasCastledBlack[0] = true;
        if (move.fromRow == 7 && move.fromCol == 0) chess->hasCastledBlack[1] = true;
    }

    chess->board[move.toRow][move.toCol] = moving;
    chess->board[move.fromRow][move.fromCol] = EMPTY;
    chess->whiteToMove = !chess->whiteToMove;
}

void unmakeMove(ChessState* chess, Move move, void* _undo) {
    UndoInfo* undo = (UndoInfo*)_undo;
    if (!undo) return;
    chess->whiteToMove = !chess->whiteToMove;
    PieceType moving = chess->board[move.toRow][move.toCol];
    if (move.isPromotion) moving = isWhite(moving) ? WHITE_PAWN : BLACK_PAWN;
    chess->board[move.fromRow][move.fromCol] = moving;
    chess->board[move.toRow][move.toCol] = undo->capturedPiece;
    if (move.isCastling) {
        int rookFromCol = (move.toCol == 6) ? 7 : 0;
        int rookToCol   = (move.toCol == 6) ? 5 : 3;
        chess->board[move.fromRow][rookFromCol] = chess->board[move.fromRow][rookToCol];
        chess->board[move.fromRow][rookToCol] = EMPTY;
    }
    if (move.isEnPassant) {
        chess->board[undo->capturedRow][undo->capturedCol] = undo->capturedPiece;
        chess->board[move.toRow][move.toCol] = EMPTY;
    }
    chess->hasCastledWhite[0] = undo->hasCastledWhite[0];
    chess->hasCastledWhite[1] = undo->hasCastledWhite[1];
    chess->hasCastledBlack[0] = undo->hasCastledBlack[0];
    chess->hasCastledBlack[1] = undo->hasCastledBlack[1];
    chess->enPassantCol = undo->enPassantCol;
}

/* minimax & findBestMove (kept simple) */
int minimax(ChessState* chess, int depth, bool whitePerspective, Engine* engine) {
    SDL_AddAtomicInt(&engine->progress.nodesSearched, 1);
    if (depth == 0) return evaluateMaterial(chess, whitePerspective);
    MoveList moves = {0};
    getAllMoves(chess, &moves);
    if (moves.count == 0) {
        if (isKingInCheck(chess, chess->whiteToMove)) return whitePerspective ? -10000 : 10000;
        return 0;
    }
    if (whitePerspective) {
        int best = -10000;
        for (int i=0;i<moves.count;i++) {
            Move m = moves.moves[i];
            UndoInfo u;
            makeMove(chess, m, &u);
            int score = minimax(chess, depth-1, !whitePerspective, engine);
            unmakeMove(chess, m, &u);
            if (score > best) best = score;
        }
        return best;
    } else {
        int best = 10000;
        for (int i=0;i<moves.count;i++) {
            Move m = moves.moves[i];
            UndoInfo u;
            makeMove(chess, m, &u);
            int score = minimax(chess, depth-1, !whitePerspective, engine);
            unmakeMove(chess, m, &u);
            if (score < best) best = score;
        }
        return best;
    }
}

Move findBestMove(ChessState* chess, int depth, Engine* engine){
    MoveList moves = {0};
    getAllMoves(chess, &moves);
    if (moves.count == 0) { Move empty = {0}; return empty; }
    Move bestMove = moves.moves[0];
    if (chess->whiteToMove) {
        int bestEval = -10000;
        for (int i=0;i<moves.count;i++){
            Move mv = moves.moves[i];
            UndoInfo u;
            makeMove(chess, mv, &u);
            int eval = minimax(chess, depth-1, false, engine);
            unmakeMove(chess, mv, &u);
            if (eval > bestEval) { bestEval = eval; bestMove = mv; }
        }
    } else {
        int bestEval = 10000;
        for (int i=0;i<moves.count;i++){
            Move mv = moves.moves[i];
            UndoInfo u;
            makeMove(chess, mv, &u);
            int eval = minimax(chess, depth-1, true, engine);
            unmakeMove(chess, mv, &u);
            if (eval < bestEval) { bestEval = eval; bestMove = mv; }
        }
    }
    return bestMove;
}

/* Engine thread: copies chess state, searches, writes result under mutex */
static int engine_thread_func(void* arg) {
    AppState* state = arg;

    SDL_SetAtomicInt(&state->engine.progress.nodesSearched, 0);
    SDL_SetAtomicInt(&state->engine.progress.depthCompleted, 0);
    SDL_SetAtomicInt(&state->engine.progress.searching, 1);

    Engine* engine = &state->engine;

    ChessState snapshot;
    SDL_LockMutex(state->engine.mutex);
    snapshot = state->chess;
    SDL_UnlockMutex(state->engine.mutex);

    Move best = {0};
    const int maxDepth = 5;

    for (int d = 1; d <= maxDepth; d++) {
        best = findBestMove(&snapshot, d, engine);
        SDL_SetAtomicInt(&state->engine.progress.depthCompleted, d);
    }

    SDL_LockMutex(state->engine.mutex);
    state->engine.resultMove = best;
    state->engine.hasMove = true;
    SDL_UnlockMutex(state->engine.mutex);

    SDL_SetAtomicInt(&state->engine.progress.searching, 0);
    return 0;
}

/* Clay render helpers */
void renderChessPiece(SDL_Texture** pieceTextures, PieceType piece, Clay_String squareIdString)
{
    if (piece == EMPTY) return;
    SDL_Texture* tex = pieceTextures[piece];
    if (!tex) return;
    char idBuf[16];
    char pieceChar = (char)(piece - '0');
    int len = SDL_snprintf(idBuf, sizeof(idBuf), "%c_%.*s", pieceChar, (int)squareIdString.length, squareIdString.chars);
    Clay_String pieceId = { .chars = idBuf, .length = len };
    CLAY(CLAY_SID(pieceId), { .aspectRatio = 1, .layout = {.sizing={.width = CLAY_SIZING_GROW(60)}}, .image = {.imageData = tex}}) {}
}

void renderChessBoard(ChessState chess, bool isWhiteView) {
    Clay_Sizing expand = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) };
    Clay_String boardId = { .chars = "Board", .length = 5 };
    CLAY(CLAY_SID(boardId), { .aspectRatio = 1, .layout = { .sizing = { .width = CLAY_SIZING_GROW(60*8) }, .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(24) } }) {
        for (char r=0;r<8;r++){
            char row = isWhiteView ? (char)(7 - r) : r;
            char rowStr[2] = { (char)('1' + row), '\0' };
            Clay_String rowIdString = { .chars = rowStr, .length = 1 };
            CLAY(CLAY_SID(rowIdString), { .aspectRatio = 1.0f/8.0f, .layout = { .sizing = { .width = CLAY_SIZING_GROW(60*8), .height = CLAY_SIZING_GROW(60) } } }) {
                for (char c=0;c<8;c++){
                    char col = isWhiteView ? (char)(7 - c) : c;
                    char squareStr[3] = { (char)('A' + col), (char)('1' + row), '\0' };
                    Clay_String squareIdString = { .chars = squareStr, .length = 2 };
                    bool light = (((int)col + (int)row) % 2) == 0;
                    bool hovered = Clay_PointerOver(CLAY_SID(squareIdString));
                    bool selected = (row == chess.selectedRow) && (col == chess.selectedCol);
                    bool moveable = isLegalMove(&chess, chess.selectedRow, chess.selectedCol, row, col);
                    int state = selected ? 1 : moveable ? 2 : hovered ? 3 : 0;
                    Clay_Color squareColor;
                    switch (state) {
                        case 1: squareColor = (Clay_Color){255,255,0,255}; break;
                        case 2: squareColor = (Clay_Color){0,0,255,255}; break;
                        case 3: squareColor = (Clay_Color){255,0,0,255}; break;
                        default: squareColor = light ? COLOR_SQUARE_WHITE : COLOR_SQUARE_BLACK; break;
                    }
                    CLAY(CLAY_SID(squareIdString), { .aspectRatio = 1, .backgroundColor = squareColor, .layout = { .sizing = expand } }) {
                        renderChessPiece(chess.pieceTextures, chess.board[row][col], squareIdString);
                    };
                }
            };
        }
    }
}

static Clay_RenderCommandArray CreateLayout(AppState* state) {
    int nodes     = SDL_GetAtomicInt(&state->engine.progress.nodesSearched);
    int depth     = SDL_GetAtomicInt(&state->engine.progress.depthCompleted);
    int searching = SDL_GetAtomicInt(&state->engine.progress.searching);
    const int maxDepth = 5;
    const int barTotalW = 300;
    const int barInnerMax = (barTotalW - 8);
    float t = 0.0f;
    if (searching && maxDepth > 0) t = (float)depth / (float)maxDepth;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int innerW = (int)(barInnerMax * t);
    Clay_BeginLayout();
    Clay_Sizing expand = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) };
    CLAY(CLAY_ID("Root"), { .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = expand }, .backgroundColor = COLOR_BG }) {
        CLAY(CLAY_ID("Toolbar"), {
            .layout = {
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(48) },
                .padding = CLAY_PADDING_ALL(12)
            },
            .backgroundColor = COLOR_TOOLBAR
        }) {
            CLAY_TEXT(CLAY_STRING("Chess"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID, .fontSize = 20, .textColor = COLOR_TEXT
            }));
            CLAY(CLAY_ID("Spacer"), { .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            CLAY(CLAY_ID("ProgressColumn"), { .layout = { .sizing = { .width = CLAY_SIZING_FIXED(barTotalW) } } }) {
                CLAY(CLAY_ID("ProgOuter"), {
                    .layout = {
                        .sizing = { .width = CLAY_SIZING_FIXED(barTotalW), .height = CLAY_SIZING_FIXED(20) },
                        .padding = CLAY_PADDING_ALL(4)
                    },
                    .backgroundColor = (Clay_Color){160,160,160,255}
                }) {
                    CLAY(CLAY_ID("ProgInner"), {
                        .layout = { .sizing = { .width = CLAY_SIZING_FIXED(innerW), .height = CLAY_SIZING_FIXED(12) } },
                        .backgroundColor = searching ? (Clay_Color){80,120,200,255} : (Clay_Color){120,120,120,255}
                    }) {}
                };
                {
                    char statusBuf[80];
                    SDL_snprintf(statusBuf, sizeof(statusBuf), "%s  depth: %d/%d  nodes: %d",
                                 searching ? "Searching" : "Idle",
                                 depth, maxDepth, nodes);
                    Clay_String statusStr = { .chars = statusBuf, .length = (int)SDL_strlen(statusBuf) };
                    CLAY_TEXT(statusStr, CLAY_TEXT_CONFIG({ .fontId = FONT_ID, .fontSize = 12, .textColor = COLOR_TEXT }));
                }
            }
        }
        CLAY(CLAY_ID("Content"), { .layout = { .sizing = expand, .padding = CLAY_PADDING_ALL(24), .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
            renderChessBoard(state->chess, true);
        }
    }

    return Clay_EndLayout();
}

/* SDL App lifecycle */
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    (void)argc; (void)argv;
    if (!TTF_Init()) return SDL_APP_FAILURE;

    AppState* state = SDL_calloc(1, sizeof(AppState));
    if (!state) return SDL_APP_FAILURE;

    if (!SDL_CreateWindowAndRenderer("SDL + Clay UI (threaded engine)", 900, 600, 0, &state->window, &state->rendererData.renderer)) {
        SDL_free(state);
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
    int w,h; SDL_GetWindowSize(state->window,&w,&h);
    Clay_Initialize(arena, (Clay_Dimensions){ (float)w, (float)h }, (Clay_ErrorHandler){ HandleClayErrors });
    Clay_SetMeasureTextFunction(SDL_MeasureText, state->rendererData.fonts);

    state->chess = initChessState();
    state->engine.mutex = SDL_CreateMutex(); /* returns SDL_Mutex* */
    state->engine.thread = NULL;
    state->engine.hasMove = false;
    state->chess.enginePending = false;

    LoadChessTextures(&state->chess, state->rendererData.renderer);

    *appstate = state;
    return SDL_APP_CONTINUE;
}

bool isCheckmate(ChessState* chess) {
    if (!isKingInCheck(chess, chess->whiteToMove)) return false;
    MoveList moves = {0}; getAllMoves(chess,&moves);
    return moves.count == 0;
}
bool isStalemate(ChessState* chess) {
    if (isKingInCheck(chess, chess->whiteToMove)) return false;
    MoveList moves = {0}; getAllMoves(chess,&moves);
    return moves.count == 0;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    AppState* state = (AppState*)appstate;
    switch (event->type) {
        case SDL_EVENT_QUIT: return SDL_APP_SUCCESS;
        case SDL_EVENT_WINDOW_RESIZED:
            Clay_SetLayoutDimensions((Clay_Dimensions){ (float)event->window.data1, (float)event->window.data2 });
            break;
        case SDL_EVENT_MOUSE_MOTION:
            Clay_SetPointerState((Clay_Vector2){ event->motion.x, event->motion.y }, event->motion.state & SDL_BUTTON_LMASK);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            Clay_SetPointerState((Clay_Vector2){ event->button.x, event->button.y }, event->button.button == SDL_BUTTON_LEFT);
            if (event->button.button == SDL_BUTTON_LEFT && state) {
                bool found = false;
                const bool whitePerspective = true;

                if (state->chess.whiteToMove == state->chess.engineWhite) return SDL_APP_CONTINUE;

                for (char r = 0; r < 8 && !found; r++) {
                    char row = whitePerspective ? (char)(7 - r) : r;
                    for (char c = 0; c < 8 && !found; c++) {
                        char col = whitePerspective ? (char)(7 - c) : c;
                        char squareStr[3] = { (char)('A' + col), (char)('1' + row), '\0' };
                        Clay_String squareIdString = { .chars = squareStr, .length = 2 };
                        if (Clay_PointerOver(CLAY_SID(squareIdString))) {
                            PieceType clickedPiece = state->chess.board[row][col];
                            int selR = state->chess.selectedRow;
                            int selC = state->chess.selectedCol;
                            bool hasSelection = (selR >= 0 && selC >= 0);

                            if (hasSelection) {
                                if (selR == row && selC == col) {
                                    state->chess.selectedRow = -1; state->chess.selectedCol = -1;
                                } else if (isLegalMove(&state->chess, selR, selC, row, col)) {
                                    PieceType moving = state->chess.board[selR][selC];
                                    Move move = { selR, selC, row, col, state->chess.board[row][col], false, false, false };
                                    if ((moving == WHITE_KING || moving == BLACK_KING) && selC == 4 && abs(col - selC) == 2) move.isCastling = true;
                                    if ((moving == WHITE_PAWN && row == 7) || (moving == BLACK_PAWN && row == 0)) move.isPromotion = true;
                                    if ((moving == WHITE_PAWN || moving == BLACK_PAWN) && selC != col && state->chess.board[row][col] == EMPTY) {
                                        move.isEnPassant = true;
                                        int capRow = isWhite(moving) ? row - 1 : row + 1;
                                        move.captured = state->chess.board[capRow][col];
                                    }

                                    UndoInfo undo;
                                    makeMove(&state->chess, move, &undo);

                                    state->chess.selectedRow = -1; state->chess.selectedCol = -1;

                                    SDL_LockMutex(state->engine.mutex);
                                    if (state->engine.thread == NULL) {
                                        state->engine.hasMove = false;
                                        state->engine.thread = SDL_CreateThread(engine_thread_func, "engine", state);
                                        if (!state->engine.thread) {
                                            SDL_Log("Failed to create engine thread: %s", SDL_GetError());
                                        }
                                    }
                                    SDL_UnlockMutex(state->engine.mutex);

                                    if (isCheckmate(&state->chess)) printf("CHECKMATE! %s wins!\n", state->chess.whiteToMove ? "Black" : "White");
                                    else if (isStalemate(&state->chess)) printf("STALEMATE! Draw.\n");
                                    else if (isKingInCheck(&state->chess, state->chess.whiteToMove)) printf("CHECK!\n");
                                } else {
                                    if (clickedPiece != EMPTY && ((state->chess.whiteToMove && !isBlack(clickedPiece)) || (!state->chess.whiteToMove && isBlack(clickedPiece)))) {
                                        state->chess.selectedRow = row; state->chess.selectedCol = col;
                                    } else {
                                        state->chess.selectedRow = -1; state->chess.selectedCol = -1;
                                    }
                                }
                            } else {
                                if (clickedPiece != EMPTY && ((state->chess.whiteToMove && !isBlack(clickedPiece)) || (!state->chess.whiteToMove && isBlack(clickedPiece)))) {
                                    state->chess.selectedRow = row; state->chess.selectedCol = col;
                                }
                            }
                            found = true;
                        }
                    }
                }
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            Clay_UpdateScrollContainers(true, (Clay_Vector2){ event->wheel.x, event->wheel.y }, 0.01f);
            break;
        default:
            break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    AppState* state = (AppState*)appstate;

    Clay_RenderCommandArray commands = CreateLayout(state);
    SDL_SetRenderDrawColor(state->rendererData.renderer, 20, 20, 20, 255);
    SDL_RenderClear(state->rendererData.renderer);
    SDL_Clay_RenderClayCommands(&state->rendererData, &commands);
    SDL_RenderPresent(state->rendererData.renderer);

    bool engineReady = false;
    Move engineMoveLocal = {0};

    SDL_LockMutex(state->engine.mutex);
    if (state->engine.hasMove) {
        engineMoveLocal = state->engine.resultMove;
        state->engine.hasMove = false;
        engineReady = true;
    }
    SDL_UnlockMutex(state->engine.mutex);

    if (engineReady) {
        UndoInfo undo;
        makeMove(&state->chess, engineMoveLocal, &undo);

        if (state->engine.thread) {
            SDL_WaitThread(state->engine.thread, NULL);
            state->engine.thread = NULL;
        }

        printf("Engine move: %s\n", move2chars(engineMoveLocal));

        if (isCheckmate(&state->chess)) printf("CHECKMATE! %s wins!\n", state->chess.whiteToMove ? "Black" : "White");
        else if (isStalemate(&state->chess)) printf("STALEMATE! Draw.\n");
        else if (isKingInCheck(&state->chess, state->chess.whiteToMove)) printf("CHECK!\n");
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    (void)result;
    AppState* state = appstate;
    if (!state) return;

    if (state->engine.thread) SDL_WaitThread(state->engine.thread, NULL);
    if (state->engine.mutex) SDL_DestroyMutex(state->engine.mutex);

    TTF_CloseFont(state->rendererData.fonts[FONT_ID]);
    SDL_free(state->rendererData.fonts);
    TTF_DestroyRendererTextEngine(state->rendererData.textEngine);
    SDL_DestroyRenderer(state->rendererData.renderer);
    SDL_DestroyWindow(state->window);
    SDL_free(state);
    TTF_Quit();
}