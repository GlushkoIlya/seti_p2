// main_single.cpp - Полная программа в одном файле
#include <iostream>
#include <string>
#include <vector>
#include <future>
#include <map>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cctype>
#include <iomanip>
#include <sstream>

using namespace std;
using json = nlohmann::json;

// Структуры данных
struct Location { string name; double lat, lon; string country; };
struct Weather { double temp; string description; };
struct Place { string title, description; double distance; };
struct LocationInfo { Location location; Weather weather; vector<Place> places; };

// Callback для CURL
size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* s) {
    size_t newLength = size * nmemb;
    try {
        s->append((char*)contents, newLength);
        return newLength;
    } catch(bad_alloc& e) { return 0; }
}

// Функция для URL кодирования
string urlEncode(const string& value) {
    ostringstream escaped;
    escaped.fill('0');
    escaped << hex;

    for (auto c : value) {
        // Сохраняем буквы, цифры и некоторые специальные символы как есть
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ',') {
            escaped << c;
        } else if (c == ' ') {
            escaped << '+';  // Пробелы заменяем на +
        } else {
            // Все остальное кодируем
            escaped << uppercase;
            escaped << '%' << setw(2) << int((unsigned char)c);
            escaped << nouppercase;
        }
    }

    return escaped.str();
}

// Простой HTTP клиент
future<string> asyncHttpRequest(const string& url) {
    return async(launch::async, [url]() -> string {
        CURL* curl = curl_easy_init();
        if (!curl) return "";
        
        string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "LocationFinder/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Разрешаем редиректы
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            cerr << "CURL error for URL " << url.substr(0, 50) << "...: " 
                 << curl_easy_strerror(res) << endl;
            response = "";
        }
        
        curl_easy_cleanup(curl);
        return response;
    });
}

// API функции
class SimpleApiClient {
public:
    static string graphhopper_key;
    static string openweather_key;
    
    static future<vector<Location>> searchLocations(const string& query) {
        return async(launch::async, [query]() {
            // Кодируем запрос для URL
            string encodedQuery = urlEncode(query);
            string url = "https://graphhopper.com/api/1/geocode?q=" + 
                        encodedQuery + "&key=" + graphhopper_key + "&limit=5";
            
            cout << "Запрос: " << url << endl;
            
            auto future = asyncHttpRequest(url);
            string response = future.get();
            
            if (response.empty()) {
                cerr << "Пустой ответ от GraphHopper API" << endl;
                return vector<Location>();
            }
            
            vector<Location> locations;
            try {
                auto j = json::parse(response);
                cout << "Ответ JSON: " << j.dump(2).substr(0, 500) << endl;
                
                if (j.contains("hits")) {
                    for (const auto& hit : j["hits"]) {
                        Location loc;
                        loc.name = hit.value("name", "");
                        if (hit.contains("point")) {
                            loc.lat = hit["point"].value("lat", 0.0);
                            loc.lon = hit["point"].value("lng", 0.0);
                        }
                        // Исправляем получение страны
                        if (hit.contains("country")) {
                            if (hit["country"].is_string()) {
                                loc.country = hit["country"].get<string>();
                            } else if (hit["country"].is_object()) {
                                loc.country = hit["country"].value("name", "");
                            }
                        }
                        if (!loc.name.empty()) {
                            cout << "Найдена локация: " << loc.name << " (" << loc.country << ")" << endl;
                            locations.push_back(loc);
                        }
                    }
                } else {
                    cerr << "Нет поля 'hits' в ответе" << endl;
                }
            } catch(const exception& e) {
                cerr << "Ошибка парсинга JSON: " << e.what() << endl;
                cerr << "Ответ был: " << response.substr(0, 500) << endl;
            }
            
            return locations;
        });
    }
    
    static future<Weather> getWeather(double lat, double lon) {
        return async(launch::async, [lat, lon]() {
            string url = "https://api.openweathermap.org/data/2.5/weather?lat=" +
                        to_string(lat) + "&lon=" + to_string(lon) +
                        "&appid=" + openweather_key + "&units=metric&lang=ru";
            
            cout << "Запрос погоды: " << url << endl;
            
            auto future = asyncHttpRequest(url);
            string response = future.get();
            
            Weather weather = {0.0, "Неизвестно"};
            
            if (response.empty()) {
                return weather;
            }
            
            try {
                auto j = json::parse(response);
                cout << "Ответ погоды JSON: " << j.dump(2).substr(0, 300) << endl;
                
                if (j.contains("main")) {
                    weather.temp = j["main"].value("temp", 0.0);
                }
                if (j.contains("weather") && !j["weather"].empty()) {
                    weather.description = j["weather"][0].value("description", "");
                }
            } catch(...) {
                cerr << "Ошибка парсинга погоды" << endl;
            }
            
            return weather;
        });
    }
    
    static future<LocationInfo> getFullInfo(const Location& loc) {
        return async(launch::async, [loc]() {
            auto weatherFuture = getWeather(loc.lat, loc.lon);
            
            LocationInfo info;
            info.location = loc;
            info.weather = weatherFuture.get();
            
            return info;
        });
    }
};

// Инициализация ключей (замените на свои реальные ключи)
string SimpleApiClient::graphhopper_key = "";
string SimpleApiClient::openweather_key = "";

// Основная программа
int main() {
    // Инициализация CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    cout << "=== Простой поиск мест ===\n";
    
    // Получаем API ключи
    cout << "Введите GraphHopper API ключ (или нажмите Enter для использования по умолчанию): ";
    string gh_key;
    getline(cin, gh_key);
    if (!gh_key.empty()) {
        SimpleApiClient::graphhopper_key = gh_key;
    } else {
        cout << "Используется ключ по умолчанию (не забудьте заменить в коде!)" << endl;
    }
    
    cout << "Введите OpenWeather API ключ (или нажмите Enter для использования по умолчанию): ";
    string ow_key;
    getline(cin, ow_key);
    if (!ow_key.empty()) {
        SimpleApiClient::openweather_key = ow_key;
    } else {
        cout << "Используется ключ по умолчанию (не забудьте заменить в коде!)" << endl;
    }
    
    cout << "\nВведите название места: ";
    string query;
    getline(cin, query);
    
    cout << "Ищем '" << query << "'...\n";
    
    try {
        auto future = SimpleApiClient::searchLocations(query);
        auto locations = future.get();
        
        if (locations.empty()) {
            cout << "Не найдено" << endl;
            return 1;
        }
        
        cout << "\nНайдено " << locations.size() << " результатов:\n";
        for (size_t i = 0; i < locations.size(); i++) {
            cout << (i+1) << ". " << locations[i].name;
            if (!locations[i].country.empty()) cout << " (" << locations[i].country << ")";
            cout << " [" << locations[i].lat << ", " << locations[i].lon << "]\n";
        }
        
        cout << "\nВыберите (1-" << locations.size() << "): ";
        int choice;
        cin >> choice;
        cin.ignore(); // Очищаем буфер после cin
        
        if (choice < 1 || choice > (int)locations.size()) {
            cout << "Неверный выбор\n";
            return 1;
        }
        
        Location selected = locations[choice-1];
        cout << "\nПолучаем погоду для " << selected.name << "...\n";
        
        auto infoFuture = SimpleApiClient::getFullInfo(selected);
        auto info = infoFuture.get();
        
        cout << "\n=== Результаты ===\n";
        cout << "Место: " << info.location.name << "\n";
        if (!info.location.country.empty()) {
            cout << "Страна: " << info.location.country << "\n";
        }
        cout << "Координаты: " << info.location.lat << ", " << info.location.lon << "\n";
        cout << "Погода: " << info.weather.description << ", " << info.weather.temp << "°C\n";
        
    } catch (const exception& e) {
        cout << "Ошибка: " << e.what() << "\n";
    }
    
    curl_global_cleanup();
    return 0;
}