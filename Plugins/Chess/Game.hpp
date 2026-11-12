#pragma once

// The rules side of the Chess plugin: one game of standard chess.
// Move generation and the draw rules come from the vendored chess-library (External/chess-library, MIT).
// This file has no NWNX dependencies so the standalone tests in Tests/ can build it on its own.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace chess { class Board; }

namespace Chess {

// Keep in sync with the NWNX_CHESS_STATUS_* constants in nwnx_chess.nss.
enum class Status : int32_t
{
    Playing          = 0,
    Check            = 1,
    Checkmate        = 2,
    Stalemate        = 3,
    DrawRepetition   = 4,
    DrawFiftyMove    = 5,
    DrawInsufficient = 6,
};

// Keep in sync with the NWNX_CHESS_COLOR_* constants in nwnx_chess.nss.
enum class Side : int32_t
{
    White = 0,
    Black = 1,
};

class Game
{
public:
    Game();
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // Sets up the position from a FEN, or the standard start position if fen is empty.
    // Returns false (and leaves the game unchanged) if the FEN is invalid.
    bool Load(const std::string& fen);

    // Plays a move in UCI notation (e2e4, e1g1 for castling, e7e8q for promotion).
    // Returns false if the move is malformed or illegal; the game is unchanged then.
    // Moves are refused once the game is over (any status other than Playing or Check).
    bool MakeMove(const std::string& uci);

    // Legal moves in UCI notation, only those starting on square if it isn't empty.
    std::vector<std::string> GetLegalMoves(const std::string& square = "") const;

    std::string GetFEN() const;
    std::string GetPGN() const;
    Status GetStatus() const;
    bool IsOver() const;
    Side GetSideToMove() const;

    // "wp", "wn", "wb", "wr", "wq", "wk" or "b?" for black, "" for an empty or invalid square.
    std::string GetPieceAt(const std::string& square) const;

    const std::string& GetStartFEN() const { return m_startFen; }
    const std::vector<std::string>& GetMoves() const { return m_uciMoves; }

    // "position fen <start> moves ..." for a UCI engine, so it knows the game's history (repetitions).
    std::string GetUciPosition() const;

private:
    std::unique_ptr<chess::Board> m_board;
    std::string m_startFen;
    bool m_standardStart = true;
    std::vector<std::string> m_uciMoves;
    std::vector<std::string> m_sanMoves;
    uint32_t m_startFullMove = 1;
    bool m_startBlack = false;
};

const char* StatusName(Status status);

}
