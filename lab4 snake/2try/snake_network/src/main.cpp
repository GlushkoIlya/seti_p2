#include <SFML/Graphics.hpp>
#include "net.h"
#include "game.h"
#include "snakes.pb.h"

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <vector>
#define MULTICAST_IP "239.192.0.4"
#define MULTICAST_PORT 9192

using namespace std;
using namespace snakes;

// ---------- Global constants ----------
const int CELL_PIXEL = 20;
const int SCOREBOARD_WIDTH = 220;

// ---------- Utility ----------
sf::Color playerColor(int id) {
    static vector<sf::Color> cols = {
        sf::Color::Red, sf::Color::Green, sf::Color::Blue, sf::Color::Yellow,
        sf::Color::Magenta, sf::Color::Cyan, sf::Color(255,140,0), sf::Color(128,0,128)
    };
    return cols[id % cols.size()];
}

// ---------- Application ----------
int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    bool as_master = false;
    string name = "player";
    if (argc>1 && string(argv[1])=="master") as_master=true;
    if (argc>2) name = argv[2];

    Net net; net.init();

    Game masterGame;
    masterGame.initDefault();
    // copy config once — we'll still lock when reading masterGame, but keep cfg for UI dims
    GameConfig cfg = masterGame.getConfig();

    uint16_t local_port = net.localPort();
    cerr << "Local unicast port: " << local_port << "\n";

    // State shared between threads/UI
    mutex state_mtx;
    GameState currentState; // last StateMsg (used on clients); on master we produce this from masterGame
    int64_t last_state_order = 0;

    // Protect access to masterGame
    mutex game_mtx;

    // Networking bookkeeping (master side)
    mutex players_mtx;
    map<int, GamePlayer> players; // id -> player
    map<int, NetAddress> playerAddr; // id -> address

    // Храним всех игроков с их рекордами (живые + умершие)
    mutex allScores_mtx;
    map<int, GamePlayer> allScores;

    atomic<int64_t> g_msg_seq(1);

    // For clients: discovered master address (ip/port)
    mutex discovered_mtx;
    NetAddress discoveredMaster{"", 0};
    atomic<bool> haveMaster(false);
    atomic<int> myPlayerId(-1);
    atomic<bool> joined(false);

    // Master id (if as_master) - создаём мастера до старта потоков
    int master_id = -1;
    if (as_master) {
        master_id = 1000; // выбранный фиксированный id для хозяина
        {
            lock_guard<mutex> g(game_mtx);
            int rc = masterGame.addLocalPlayer(name, master_id);
            if (rc != 0) {
                cerr << "Warning: failed to place master snake (rc=" << rc << ")\n";
            }
        }
        GamePlayer gp;
        gp.set_id(master_id);
        gp.set_name(name);
        gp.set_role(NodeRole::MASTER);
        gp.set_score(0);
        gp.set_ip_address("127.0.0.1");
        gp.set_port(local_port);
        {
            lock_guard<mutex> lg(players_mtx);
            players[master_id] = gp;
            playerAddr[master_id] = NetAddress{"127.0.0.1", local_port};
        }
        // Добавляем в allScores сразу мастера
        {
            lock_guard<mutex> lg(allScores_mtx);
            allScores[master_id] = gp;
        }
    }

    // Receiver thread (reads messages on multicast and unicast)
    thread receiver([&](){
        while (true) {
            GameMessage gm; NetAddress sender;
            if (!net.recv(gm, sender)) continue;

            // ANNOUNCEMENT
            if (gm.has_announcement()) {
                lock_guard<mutex> lg(discovered_mtx);
                if (!haveMaster.load()) {
                    discoveredMaster = sender;
                    haveMaster.store(true);
                    cerr << "Discovered master at " << sender.ip << ":" << sender.port << "\n";
                }
            }
            // JOIN
            else if (gm.has_join()) {
                if (as_master) {
                    const auto& j = gm.join();
                    int w, h;
                    { // read config safely
                        lock_guard<mutex> g(game_mtx);
                        w = masterGame.getConfig().width();
                        h = masterGame.getConfig().height();
                    }
                    bool placed = false;
                    int new_id = 2000 + (int)(chrono::system_clock::now().time_since_epoch().count()%100000);
                    for (int yy=0; yy<h && !placed; ++yy) {
                        for (int xx=0; xx<w && !placed; ++xx) {
                            bool ok = true;
                            vector<Coord> cells;
                            for (int dy=0; dy<5 && ok; ++dy) for (int dx=0; dx<5 && ok; ++dx) {
                                int cx = (xx+dx)%w, cy = (yy+dy)%h;
                                cells.push_back({cx,cy});
                            }
                            ok = true;
                            GameState gs;
                            { lock_guard<mutex> g(game_mtx); gs = masterGame.toProto(0); }
                            for (auto &c: cells) {
                                bool occ=false;
                                for (int si=0; si<gs.snakes_size(); ++si) {
                                    const auto &s = gs.snakes(si);
                                    for (int pi=0; pi<s.points_size(); ++pi) {
                                        const auto &p = s.points(pi);
                                        if (p.x()==c.x && p.y()==c.y) { occ=true; break; }
                                    }
                                    if (occ) break;
                                }
                                for (int fi=0; fi<gs.foods_size(); ++fi) {
                                    const auto &f = gs.foods(fi);
                                    if (f.x()==c.x && f.y()==c.y) { occ=true; break; }
                                }
                                if (occ) { ok = false; break; }
                            }
                            if (!ok) continue;

                            int cx = (xx+2)%w, cy = (yy+2)%h;
                            Coord head{cx,cy};
                            vector<Coord> nbrs = {{(cx+1)%w,cy},{(cx-1+w)%w,cy},{cx,(cy+1)%h},{cx,(cy-1+h)%h}};
                            Coord tail = nbrs[rand()%4];

                            bool bad=false;
                            for (int fi=0; fi<gs.foods_size(); ++fi) {
                                const auto &f = gs.foods(fi);
                                if ((f.x()==head.x && f.y()==head.y) || (f.x()==tail.x && f.y()==tail.y)) { bad=true; break; }
                            }
                            if (bad) continue;

                            int rc;
                            {
                                lock_guard<mutex> g(game_mtx);
                                rc = masterGame.addLocalPlayer(j.player_name(), new_id);
                            }
                            if (rc==0) {
                                GamePlayer gp; gp.set_id(new_id); gp.set_name(j.player_name()); gp.set_role(NodeRole::NORMAL); gp.set_score(0);
                                gp.set_ip_address(sender.ip); gp.set_port(sender.port);
                                {
                                    lock_guard<mutex> lg(players_mtx);
                                    players[new_id] = gp;
                                    playerAddr[new_id] = sender;
                                }
                                // добавляем в allScores с 0 очков
                                {
                                    lock_guard<mutex> lg(allScores_mtx);
                                    allScores[new_id] = gp;
                                }
                                GameMessage ack; ack.set_msg_seq(g_msg_seq++);
                                ack.set_receiver_id(new_id);
                                ack.mutable_ack();
                                net.sendTo(ack, sender);
                                cerr << "Accepted join from " << j.player_name() << " id=" << new_id << " addr=" << sender.ip << ":" << sender.port << "\n";
                                placed = true;
                                break;
                            }
                        }
                    }
                    if (!placed) {
                        GameMessage err; err.set_msg_seq(g_msg_seq++);
                        err.mutable_error()->set_error_message("No place available");
                        net.sendTo(err, sender);
                    }
                }
            }
            // STEER
            else if (gm.has_steer()) {
                if (as_master) {
                    int sid = gm.sender_id();
                    if (sid==0) {
                        lock_guard<mutex> lg(players_mtx);
                        for (auto &pr: players) {
                            if (pr.second.ip_address() == sender.ip && pr.second.port() == (int)sender.port) { sid = pr.first; break; }
                        }
                    }
                    if (sid!=0) {
                        { lock_guard<mutex> g(game_mtx); masterGame.steer(sid, gm.steer().direction()); }
                        GameMessage ack; ack.set_msg_seq(g_msg_seq++); ack.set_receiver_id(sid); ack.mutable_ack();
                        net.sendTo(ack, sender);
                    }
                }
            }
            // STATE (clients receive)
            else if (gm.has_state()) {
                GameMessage ack; ack.set_msg_seq(g_msg_seq++); ack.set_receiver_id(gm.state().state().state_order()); ack.mutable_ack();
                net.sendTo(ack, sender);

                const GameState& st = gm.state().state();
                lock_guard<mutex> lg(state_mtx);
                if (st.state_order() > last_state_order) {
                    currentState = st;
                    last_state_order = st.state_order();
                }
            }
            // ACK (client receives ack for its Join)
            else if (gm.has_ack()) {
                if (!as_master) {
                    int rid = gm.receiver_id();
                    if (rid>0) {
                        myPlayerId.store(rid);
                        joined.store(true);
                        cerr << "Joined game, my id = " << rid << "\n";
                    }
                }
            }
        }
    });
    receiver.detach();

    // Announcer (MASTER): send AnnouncementMsg every 1s
    thread announcer;
    if (as_master) {
        announcer = thread([&](){
            while (true) {
                GameMessage gm; gm.set_msg_seq(g_msg_seq++);
                GameAnnouncement* ga = gm.mutable_announcement()->add_games();
                GameConfig localCfg;
                { lock_guard<mutex> g(game_mtx); localCfg = masterGame.getConfig(); }
                *ga->mutable_config() = localCfg;
                GamePlayers* gps = ga->mutable_players();
                {
                    lock_guard<mutex> lg(players_mtx);
                    for (auto &pr: players) *gps->add_players() = pr.second;
                }
                ga->set_can_join(true);
                ga->set_game_name(name);
                net.sendAnnouncement(gm);
                this_thread::sleep_for(chrono::seconds(1));
            }
        });
        announcer.detach();
    } else {
        // Clients: send Discover once to find masters
        GameMessage disc; disc.set_msg_seq(g_msg_seq++); disc.mutable_discover();
        net.sendTo(disc, NetAddress{string(MULTICAST_IP), (uint16_t)MULTICAST_PORT});
    }

    // Tick thread (MASTER): advances game and sends StateMsg to all players
    thread tickThread;
    if (as_master) {
        tickThread = thread([&](){
            int64_t state_order = 1;
            while (true) {
                int delay;
                { lock_guard<mutex> g(game_mtx); delay = masterGame.getConfig().state_delay_ms(); }
                this_thread::sleep_for(chrono::milliseconds(delay));
                vector<int> ate, died;
                { lock_guard<mutex> g(game_mtx); masterGame.tick(ate, died); }

                // Обновляем очки живых игроков
                {
                    lock_guard<mutex> lg(players_mtx);
                    for (int id: ate) {
                        if (players.count(id)) players[id].set_score(players[id].score() + 1);
                    }
                }

                // Обновляем allScores рекорды
                {
                    lock_guard<mutex> lg(players_mtx);
                    lock_guard<mutex> lg2(allScores_mtx);
                    for (int id : ate) {
                        if (players.count(id)) {
                            int newScore = players[id].score();
                            if (!allScores.count(id) || allScores[id].score() < newScore) {
                                GamePlayer gp = players[id];
                                gp.set_score(newScore);
                                allScores[id] = gp;
                            }
                        }
                    }
                }

                // remove players whose snakes died, но сохраняем их рекорды
                if (!died.empty()) {
                    lock_guard<mutex> lg(players_mtx);
                    lock_guard<mutex> lg2(allScores_mtx);
                    for (int did : died) {
                        if (players.count(did)) {
                            if (allScores.count(did)) {
                                if (allScores[did].score() < players[did].score()) {
                                    allScores[did].set_score(players[did].score());
                                }
                            } else {
                                allScores[did] = players[did];
                            }
                            players.erase(did);
                        }
                        if (playerAddr.count(did)) playerAddr.erase(did);
                        cerr << "Player " << did << " removed after death\n";
                    }
                }

                GameState gs;
                {
                    lock_guard<mutex> g(game_mtx);
                    gs = masterGame.toProto(state_order++);
                }
                GamePlayers* gps = gs.mutable_players();
                {
                    lock_guard<mutex> lg(players_mtx);
                    for (auto &pr: players) *gps->add_players() = pr.second;
                }
                GameMessage gm; gm.set_msg_seq(g_msg_seq++);
                gm.mutable_state()->mutable_state()->CopyFrom(gs);

                vector<pair<int,NetAddress>> recipients;
                {
                    lock_guard<mutex> lg(players_mtx);
                    for (auto &pr: playerAddr) recipients.push_back(pr);
                }
                for (auto &p : recipients) net.sendTo(gm, p.second);

                {
                    lock_guard<mutex> lg(state_mtx);
                    currentState = gs;
                    last_state_order = gs.state_order();
                }
            }
        });
        tickThread.detach();
    } else {
        // Client: after discovering master, send Join
        for (int i=0;i<20 && !haveMaster.load();++i) this_thread::sleep_for(chrono::milliseconds(200));
        if (haveMaster.load()) {
            NetAddress m;
            { lock_guard<mutex> lg(discovered_mtx); m = discoveredMaster; }
            GameMessage join; join.set_msg_seq(g_msg_seq++);
            join.mutable_join()->set_player_name(name);
            join.mutable_join()->set_game_name("my_game");
            join.mutable_join()->set_requested_role(NodeRole::NORMAL);
            net.sendTo(join, m);
            cerr << "Sent Join to " << m.ip << ":" << m.port << "\n";
        } else {
            cerr << "No master discovered\n";
        }
    }

    // Setup SFML window
    int winW = cfg.width() * CELL_PIXEL + SCOREBOARD_WIDTH;
    int winH = cfg.height() * CELL_PIXEL;
    sf::RenderWindow window(sf::VideoMode(winW, winH), as_master ? ("Snake (MASTER) " + name) : ("Snake (CLIENT) " + name));
    sf::Font font;
    if (!font.loadFromFile("DejaVuSans.ttf")) {
        cerr << "Failed to load DejaVuSans.ttf; leaderboard text may not render\n";
    }

    // Main UI loop
    while (window.isOpen()) {
        sf::Event ev;
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) { window.close(); break; }
            if (ev.type == sf::Event::KeyPressed) {
                Direction d;
                bool send = false;
                if (ev.key.code == sf::Keyboard::W) { d = Direction::UP; send = true; }
                else if (ev.key.code == sf::Keyboard::S) { d = Direction::DOWN; send = true; }
                else if (ev.key.code == sf::Keyboard::A) { d = Direction::LEFT; send = true; }
                else if (ev.key.code == sf::Keyboard::D) { d = Direction::RIGHT; send = true; }

                if (send) {
                    if (as_master) {
                        if (master_id != -1) {
                            lock_guard<mutex> g(game_mtx);
                            masterGame.steer(master_id, d);
                        }
                    } else {
                        if (haveMaster.load()) {
                            NetAddress m;
                            { lock_guard<mutex> lg(discovered_mtx); m = discoveredMaster; }
                            GameMessage sm; sm.set_msg_seq(g_msg_seq++);
                            if (myPlayerId.load()>0) sm.set_sender_id(myPlayerId.load());
                            sm.mutable_steer()->set_direction(d);
                            net.sendTo(sm, m);
                        }
                    }
                }
            }
        }

        // render
        window.clear(sf::Color(30,30,30));

        GameState snapshot;
        {
            lock_guard<mutex> lg(state_mtx);
            snapshot = currentState;
        }
        if (as_master) {
            lock_guard<mutex> g(game_mtx);
            snapshot = masterGame.toProto(last_state_order+1);
        }

        sf::RectangleShape cell(sf::Vector2f((float)CELL_PIXEL-1.0f, (float)CELL_PIXEL-1.0f));

        // Draw foods
        for (int i=0;i<snapshot.foods_size();++i) {
            auto f = snapshot.foods(i);
            cell.setPosition(f.x()*CELL_PIXEL, f.y()*CELL_PIXEL);
            cell.setFillColor(sf::Color::Red);
            window.draw(cell);
        }

        // Draw snakes
        for (int si=0; si<snapshot.snakes_size(); ++si) {
            const auto& s = snapshot.snakes(si);
            int pid = s.player_id();
            sf::Color col = playerColor(pid);
            for (int pi=0; pi<s.points_size(); ++pi) {
                auto p = s.points(pi);
                cell.setPosition(p.x()*CELL_PIXEL, p.y()*CELL_PIXEL);
                if (pi==0) cell.setFillColor(col);
                else cell.setFillColor(sf::Color(std::max(0, col.r/2), std::max(0, col.g/2), std::max(0, col.b/2)));
                window.draw(cell);
            }
        }

        // draw separator
        sf::RectangleShape sep(sf::Vector2f(2.0f, (float)winH));
        sep.setPosition((float)(cfg.width()*CELL_PIXEL), 0);
        sep.setFillColor(sf::Color(80,80,80));
        window.draw(sep);

        // draw leaderboard
        sf::Text title("Leaderboard", font, 20);
        title.setFillColor(sf::Color::White);
        title.setPosition((float)(cfg.width()*CELL_PIXEL + 10), 5);
        window.draw(title);

        vector<pair<string,int>> scores;

        if (as_master) {
            lock_guard<mutex> lg(allScores_mtx);
            for (auto& [id, gp] : allScores) {
                scores.push_back({gp.name(), gp.score()});
            }
        } else {
            for (int i=0;i<snapshot.players().players_size(); ++i) {
                const auto &gp = snapshot.players().players(i);
                scores.push_back({gp.name(), gp.score()});
            }
        }
        sort(scores.begin(), scores.end(), [](const auto&a,const auto&b){ return a.second > b.second; });

        int y = 40;
        for (auto &kv: scores) {
            sf::Text line((kv.first + " : " + to_string(kv.second)), font, 18);
            line.setFillColor(sf::Color::White);
            line.setPosition((float)(cfg.width()*CELL_PIXEL + 10), (float)y);
            window.draw(line);
            y += 24;
        }

        if (!as_master && !joined.load()) {
            sf::Text st("Connecting...", font, 18);
            st.setFillColor(sf::Color::Yellow);
            st.setPosition((float)(10), (float)(cfg.height()*CELL_PIXEL - 30));
            window.draw(st);
        }

        window.display();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
