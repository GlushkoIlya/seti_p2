#include "game.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <iostream>
#include <random> // <- нужно для placeFood и преобразований в еду

Game::Game() { initDefault(); }

void Game::initDefault() {
    config_.set_width(40);
    config_.set_height(20);
    config_.set_food_static(3);
    config_.set_state_delay_ms(100);
    snakes_.clear(); foods_.clear(); placeFood();
}

GameConfig Game::getConfig() const { return config_; }

void Game::placeFood() {
    std::mt19937 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> wx(0, config_.width()-1), wy(0, config_.height()-1);

    int target = config_.food_static();
    int attempts = 0;
    while ((int)foods_.size() < target && attempts < 10000) {
        Coord c{wx(rng), wy(rng)};
        bool ok=true;
        for (auto &f: foods_) if (coordEquals(f,c)) { ok=false; break; }
        if (!ok) { attempts++; continue; }
        bool onSnake=false;
        for (auto &s: snakes_) for (auto &p: s.second.points) if (coordEquals(p,c)) onSnake=true;
        if (!onSnake) foods_.push_back(c);
        attempts++;
    }
}

int Game::addLocalPlayer(const std::string& /*name*/, int id) {
    SnakeInfo s; s.player_id = id; s.points.clear();
    int w=config_.width(), h=config_.height();
    bool placed=false;
    for (int y=0;y<h && !placed;y++) for (int x=0;x<w && !placed;x++) {
        Coord c1{x,y}, c2{(x+1)%w,y};
        bool ok=true;
        for (auto &sn: snakes_) for (auto&p: sn.second.points) if (coordEquals(p,c1) || coordEquals(p,c2)) ok=false;
        for (auto &f: foods_) if (coordEquals(f,c1) || coordEquals(f,c2)) ok=false;
        if (!ok) continue;
        s.points.push_back(c1); s.points.push_back(c2); s.dir = Direction::RIGHT; s.alive=true;
        snakes_[id]=s; placed=true; break;
    }
    if (!placed) return -1;
    return 0;
}

int Game::removeLocalPlayer(int id) {
    auto it = snakes_.find(id);
    if (it != snakes_.end()) {
        snakes_.erase(it);
        return 0;
    }
    return -1;
}


bool Game::steer(int player_id, Direction d) {
    auto it = snakes_.find(player_id);
    if (it==snakes_.end()) return false;
    auto &s = it->second;
    if ((s.dir==Direction::LEFT && d==Direction::RIGHT) || (s.dir==Direction::RIGHT && d==Direction::LEFT) ||
        (s.dir==Direction::UP && d==Direction::DOWN) || (s.dir==Direction::DOWN && d==Direction::UP)) return false;
    s.dir = d; return true;
}

bool Game::isCellOccupiedBySnake(int x,int y) const {
    for (auto &pr: snakes_) for (auto &p: pr.second.points) if (p.x==x && p.y==y) return true;
    return false;
}

void Game::tick(std::vector<int>& ate_ids, std::vector<int>& died_ids) {
    ate_ids.clear(); died_ids.clear();
    if (snakes_.empty()) { placeFood(); return; }

    int w = config_.width(), h = config_.height();
    struct NewSnake { int id; std::vector<Coord> newpoints; Direction dir; bool wasAlive; bool ateFood; };
    std::unordered_map<int, NewSnake> newSnakes;

    auto foodIndex = [&](const Coord& c)->int{
        for (size_t i=0;i<foods_.size();++i) if (coordEquals(foods_[i], c)) return (int)i;
        return -1;
    };

    // 1) compute new heads/bodies
    for (auto &pr: snakes_) {
        auto s = pr.second; // copy
        NewSnake ns; ns.id = s.player_id; ns.dir = s.dir; ns.wasAlive = s.alive; ns.ateFood=false;
        Coord head = s.points.front();
        switch (s.dir) {
            case Direction::UP: head.y = (head.y - 1 + h) % h; break;
            case Direction::DOWN: head.y = (head.y + 1) % h; break;
            case Direction::LEFT: head.x = (head.x - 1 + w) % w; break;
            case Direction::RIGHT: head.x = (head.x + 1) % w; break;
        }
        ns.newpoints = s.points;
        ns.newpoints.insert(ns.newpoints.begin(), head);

        int fi = foodIndex(head);
        if (fi != -1) {
            ns.ateFood = true;
        } else {
            if (!ns.newpoints.empty()) ns.newpoints.pop_back();
        }
        newSnakes[ns.id] = ns;
    }

    // 2) build occupancy
    struct CellOccupant { std::vector<int> heads; std::vector<int> bodies; };
    std::map<std::pair<int,int>, CellOccupant> occ;
    for (auto &pr: newSnakes) {
        const auto &ns = pr.second;
        if (ns.newpoints.empty()) continue;
        auto hcoord = ns.newpoints.front();
        occ[{hcoord.x, hcoord.y}].heads.push_back(ns.id);
        for (size_t i=1;i<ns.newpoints.size();++i) occ[{ns.newpoints[i].x, ns.newpoints[i].y}].bodies.push_back(ns.id);
    }

    // 3) collisions
    std::set<int> willDie;
    for (auto &c : occ) {
        if (c.second.heads.size() > 1) {
            for (int pid: c.second.heads) willDie.insert(pid);
        }
    }
    for (auto &c : occ) {
        if (!c.second.heads.empty() && !c.second.bodies.empty()) {
            for (int pid: c.second.heads) willDie.insert(pid);
        }
    }

    // victim gains (not applied to scores here, caller may use it)
    std::multimap<int,int> victimGains;
    for (auto &c : occ) {
        if (!c.second.heads.empty() && !c.second.bodies.empty()) {
            for (int hitter : c.second.heads) {
                for (int victim : c.second.bodies) {
                    victimGains.insert({victim, 1});
                }
            }
        }
    }

    // 4) compute eaten foods set and remove them
    std::set<std::pair<int,int>> foodEatenPositions;
    for (auto &kv : newSnakes) {
        if (kv.second.ateFood && !kv.second.newpoints.empty()) {
            auto c = kv.second.newpoints.front();
            foodEatenPositions.insert({c.x,c.y});
        }
    }
    std::vector<Coord> newFoods;
    for (auto &f : foods_) {
        if (foodEatenPositions.count({f.x,f.y})==0) newFoods.push_back(f);
    }
    foods_.swap(newFoods);

    // 5) apply newSnakes to snakes_ (mark died or update)
    for (auto &kv : newSnakes) {
        int id = kv.first;
        if (willDie.count(id)) {
            snakes_[id].points = kv.second.newpoints;
            snakes_[id].dir = kv.second.dir;
            snakes_[id].alive = false; // mark dead — will be fully removed below (and turned into food)
            died_ids.push_back(id);
        } else {
            snakes_[id].points = kv.second.newpoints;
            snakes_[id].dir = kv.second.dir;
            snakes_[id].alive = true;
            if (kv.second.ateFood) ate_ids.push_back(id);
        }
    }

    // 6) log victim gains (caller may use)
    for (auto it = victimGains.begin(); it != victimGains.end(); ++it) {
        int victim = it->first;
        if (std::find(died_ids.begin(), died_ids.end(), victim) == died_ids.end()) {
            std::cerr << "Victim " << victim << " was crashed into and should get +1 (if not dead itself)\n";
        }
    }

    // 7) for each died snake, convert some cells to food with p=0.5, otherwise empty
    std::mt19937 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::bernoulli_distribution half(0.5);
    // collect cells to add to foods
    std::vector<Coord> foodsToAdd;
    for (int did : died_ids) {
        auto it = snakes_.find(did);
        if (it == snakes_.end()) continue;
        auto &sinfo = it->second;
        for (auto &p: sinfo.points) {
            if (half(rng)) {
                // check not occupied by other alive snake (just in case)
                bool onSnake = false;
                for (auto &pr: snakes_) {
                    if (pr.first==did) continue;
                    for (auto &pp: pr.second.points) if (coordEquals(pp,p)) onSnake=true;
                }
                if (!onSnake) foodsToAdd.push_back(p);
            }
        }
    }
    // add new foods (deduplicate)
    for (auto &f : foodsToAdd) {
        bool exists=false;
        for (auto &ff: foods_) if (coordEquals(ff,f)) { exists=true; break; }
        if (!exists) foods_.push_back(f);
    }

    // 8) remove died snakes from map (they're dead — cells already converted above)
    for (int did : died_ids) {
        snakes_.erase(did);
    }

    // 9) ensure minimal food present
    placeFood();
}

GameState Game::toProto(int64_t state_order) const {
    GameState gs; gs.set_state_order((int)state_order);
    for (auto &pr: snakes_) {
        GameState::Snake s;
        s.set_player_id(pr.second.player_id);
        s.set_state(GameState::Snake::ALIVE); // all snakes that remain are alive in map
        s.set_head_direction(pr.second.dir);
        for (auto &p: pr.second.points) {
            GameState::Coord* c = s.add_points(); c->set_x(p.x); c->set_y(p.y);
        }
        *gs.add_snakes() = s;
    }
    for (auto &f: foods_) { GameState::Coord* c = gs.add_foods(); c->set_x(f.x); c->set_y(f.y); }
    GamePlayers* gps = gs.mutable_players();
    (void)gps;
    return gs;
}
