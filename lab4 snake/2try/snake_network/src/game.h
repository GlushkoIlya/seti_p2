#pragma once
#include "snakes.pb.h"
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <random>

using namespace snakes;

struct Coord { int x; int y; };

class Game {
public:
    Game();
    void initDefault();
    void placeFood(); // ensure at least food_static present (subject to field full)
    int addLocalPlayer(const std::string& name, int id); // place 2-cell snake for new player
    int removeLocalPlayer(int id);
    bool steer(int player_id, Direction d);

    // execute one tick: fills ate_ids and died_ids
    void tick(std::vector<int>& ate_ids, std::vector<int>& died_ids);

    GameState toProto(int64_t state_order) const;
    GameConfig getConfig() const;

private:
    GameConfig config_;
    struct SnakeInfo { int player_id; std::vector<Coord> points; Direction dir; bool alive; };
    std::unordered_map<int, SnakeInfo> snakes_;
    std::vector<Coord> foods_;
    bool coordEquals(const Coord&a,const Coord&b) const { return a.x==b.x && a.y==b.y; }
    bool isCellOccupiedBySnake(int x,int y) const;
};
