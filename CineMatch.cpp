#include <iostream>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <set>
#include <algorithm>
#include <random>
#include <cstring>

using json = nlohmann::json;
const std::string API_KEY = "e059ee50571ca68d3761e10bbc575257";

enum EstadoApp { LOGIN, SELECCION_INICIAL, DASHBOARD };
enum VistaDashboard { MEZCLA, SOLO_PELIS, SOLO_SERIES };

EstadoApp estadoActual = LOGIN;
VistaDashboard vistaActual = MEZCLA;

struct Resultado { 
    int id; 
    std::string titulo; 
    float rating; 
    bool esSerie; 
};

// --- VARIABLES GLOBALES ---
std::vector<Resultado> opcionesSeleccion;
std::vector<Resultado> resultadosPantalla;
std::vector<Resultado> misFavoritos;
std::set<int> elegidas;
sqlite3* db;
bool viendoFavoritos = false;

// --- GESTIÓN DE BASE DE DATOS ---
void initDB() {
    if (sqlite3_open("cinematch.db", &db) == SQLITE_OK) {
        const char* sql = "CREATE TABLE IF NOT EXISTS favoritos("
                          "id INTEGER PRIMARY KEY, "
                          "titulo TEXT, "
                          "esSerie INTEGER);";
        sqlite3_exec(db, sql, 0, 0, 0);
    }
}

int contarFavoritosEnDB() {
    int conteo = 0;
    sqlite3_stmt* stmt;
    const char* sql = "SELECT COUNT(*) FROM favoritos;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            conteo = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return conteo;
}

void guardarFavorito(int id, std::string titulo, bool esSerie) {
    std::string sql = "INSERT OR REPLACE INTO favoritos (id, titulo, esSerie) VALUES (" +
                      std::to_string(id) + ", '" + titulo + "', " + (esSerie ? "1" : "0") + ");";
    sqlite3_exec(db, sql.c_str(), 0, 0, 0);
}

void eliminarFavorito(int id) {
    std::string sql = "DELETE FROM favoritos WHERE id = " + std::to_string(id) + ";";
    sqlite3_exec(db, sql.c_str(), 0, 0, 0);
}

void refrescarFavoritos() {
    misFavoritos.clear();
    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, titulo, esSerie FROM favoritos;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            std::string titulo = (const char*)sqlite3_column_text(stmt, 1);
            bool esSerie = sqlite3_column_int(stmt, 2) == 1;
            misFavoritos.push_back({id, titulo, 0.0f, esSerie});
        }
    }
    sqlite3_finalize(stmt);
}

// --- UTILIDADES DE RED ---
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::vector<Resultado> pedirAPI(std::string url, bool esSerie) {
    CURL* curl = curl_easy_init();
    std::string rb;
    std::vector<Resultado> res;
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        if(curl_easy_perform(curl) == CURLE_OK) {
            try {
                auto datos = json::parse(rb);
                for(auto& item : datos["results"]) {
                    std::string n = item.contains("title") ? item["title"] : item["name"];
                    res.push_back({item["id"], n, item.value("vote_average", 0.0f), esSerie});
                }
            } catch (...) {}
        }
        curl_easy_cleanup(curl);
    }
    return res;
}

// --- AUTENTICACIÓN REAL TMDB ---
bool validarCredencialesTMDB(std::string usuario, std::string password) {
    CURL* curl = curl_easy_init();
    std::string rb;
    bool exito = false;
    if (curl) {
        // 1. Obtener Request Token
        std::string urlToken = "https://api.themoviedb.org/3/authentication/token/new?api_key=" + API_KEY;
        curl_easy_setopt(curl, CURLOPT_URL, urlToken.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        
        if (curl_easy_perform(curl) == CURLE_OK) {
            auto jToken = json::parse(rb);
            std::string token = jToken["request_token"];
            rb.clear();

            // 2. Validar con Login
            std::string urlLogin = "https://api.themoviedb.org/3/authentication/token/validate_with_login?api_key=" + API_KEY;
            json body = {{"username", usuario}, {"password", password}, {"request_token", token}};
            std::string js = body.dump();

            curl_easy_setopt(curl, CURLOPT_URL, urlLogin.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, js.c_str());
            struct curl_slist* h = curl_slist_append(NULL, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);

            if (curl_easy_perform(curl) == CURLE_OK) {
                auto jRes = json::parse(rb);
                exito = jRes.value("success", false);
            }
            curl_slist_free_all(h);
        }
        curl_easy_cleanup(curl);
    }
    return exito;
}

void cargarDashboard(VistaDashboard vista) {
    resultadosPantalla.clear();
    std::string base = "https://api.themoviedb.org/3/discover/";
    std::string params = "?api_key="+API_KEY+"&vote_average.gte=7.5&language=es-ES&sort_by=popularity.desc";

    if(vista == MEZCLA || vista == SOLO_PELIS) {
        auto p = pedirAPI(base + "movie" + params, false);
        if (p.size() > 10) p.resize(10);
        resultadosPantalla.insert(resultadosPantalla.end(), p.begin(), p.end());
    }
    if(vista == MEZCLA || vista == SOLO_SERIES) {
        auto s = pedirAPI(base + "tv" + params, true);
        if (s.size() > 10) s.resize(10);
        resultadosPantalla.insert(resultadosPantalla.end(), s.begin(), s.end());
    }
    if(vista == MEZCLA) {
        std::shuffle(resultadosPantalla.begin(), resultadosPantalla.end(), std::mt19937(std::random_device()()));
        if (resultadosPantalla.size() > 10) resultadosPantalla.resize(10);
    }
}

// --- PANTALLAS ---
void PantallaLogin() {
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Login", nullptr, ImGuiWindowFlags_NoTitleBar);
    
    float sw = ImGui::GetIO().DisplaySize.x, sh = ImGui::GetIO().DisplaySize.y;
    ImGui::SetCursorPosY(sh * 0.25f); ImGui::SetWindowFontScale(4.0f);
    ImGui::SetCursorPosX((sw - ImGui::CalcTextSize("CINEMATCH").x) * 0.5f);
    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.1f, 1.0f), "CINEMATCH"); ImGui::SetWindowFontScale(1.0f);

    static char u[64] = "", p[64] = "";
    static std::string err = "";
    static bool loading = false;

    ImGui::SetCursorPosY(sh * 0.45f);
    ImGui::SetCursorPosX((sw - 300) * 0.5f); ImGui::InputTextWithHint("##u", "Usuario TMDB", u, 64);
    ImGui::SetCursorPosX((sw - 300) * 0.5f); ImGui::InputTextWithHint("##p", "Contraseña", p, 64, ImGuiInputTextFlags_Password);

    ImGui::SetCursorPosX((sw - 120) * 0.5f);
    if (loading) ImGui::Text("Validando en TMDB...");
    else {
        if (ImGui::Button("ENTRAR", ImVec2(120, 40))) {
            if (strlen(u) > 0 && strlen(p) > 0) {
                loading = true;
                if (validarCredencialesTMDB(u, p)) {
                    if (contarFavoritosEnDB() >= 5) {
                        cargarDashboard(MEZCLA);
                        refrescarFavoritos();
                        estadoActual = DASHBOARD;
                    } else {
                        opcionesSeleccion = pedirAPI("https://api.themoviedb.org/3/movie/top_rated?api_key="+API_KEY+"&language=es-ES", false);
                        estadoActual = SELECCION_INICIAL;
                    }
                } else err = "Usuario o contraseña de TMDB incorrectos.";
                loading = false;
            } else err = "Por favor, rellena todos los campos.";
        }
    }
    if (!err.empty()) {
        ImGui::SetCursorPosX((sw - ImGui::CalcTextSize(err.c_str()).x) * 0.5f);
        ImGui::TextColored(ImVec4(1,0.2f,0.2f,1), "%s", err.c_str());
    }
    ImGui::End();
}

void PantallaSeleccion() {
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Seleccion Inicial", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::Text("Paso 1: Elige 5 títulos favoritos:"); ImGui::Separator();
    
    ImGui::BeginChild("FullList", ImVec2(0, -60), true);
    for (auto& o : opcionesSeleccion) {
        bool s = elegidas.count(o.id);
        if (ImGui::Selectable(o.titulo.c_str(), s)) { if(s) elegidas.erase(o.id); else elegidas.insert(o.id); }
    }
    ImGui::EndChild();
    
    ImGui::Text("Seleccionados: %zu / 5", elegidas.size());
    ImGui::SameLine();
    if (elegidas.size() >= 5 && ImGui::Button("FINALIZAR", ImVec2(120, 35))) {
        for (int id : elegidas) {
            for (auto& o : opcionesSeleccion) {
                if (o.id == id) { guardarFavorito(o.id, o.titulo, o.esSerie); break; }
            }
        }
        cargarDashboard(MEZCLA);
        refrescarFavoritos();
        estadoActual = DASHBOARD;
    }
    ImGui::End();
}

void PantallaDashboard() {
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Dashboard", nullptr, ImGuiWindowFlags_NoTitleBar);

    ImGui::BeginChild("Sidebar", ImVec2(200, 0), true);
    ImGui::Text("MENÚ"); ImGui::Separator();
    if (ImGui::Selectable("Recomendaciones", !viendoFavoritos && vistaActual == MEZCLA)) { viendoFavoritos = false; vistaActual = MEZCLA; cargarDashboard(MEZCLA); }
    if (ImGui::Selectable("Solo Películas", !viendoFavoritos && vistaActual == SOLO_PELIS)) { viendoFavoritos = false; vistaActual = SOLO_PELIS; cargarDashboard(SOLO_PELIS); }
    if (ImGui::Selectable("Solo Series", !viendoFavoritos && vistaActual == SOLO_SERIES)) { viendoFavoritos = false; vistaActual = SOLO_SERIES; cargarDashboard(SOLO_SERIES); }
    ImGui::Spacing(); ImGui::Separator();
    if (ImGui::Selectable("Mis Favoritos", viendoFavoritos)) { viendoFavoritos = true; refrescarFavoritos(); }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginGroup();
    if (!viendoFavoritos) {
        if (ImGui::Button("Refrescar Catálogo")) cargarDashboard(vistaActual);
        ImGui::Separator();
        ImGui::BeginChild("View");
        for (auto& r : resultadosPantalla) {
            ImGui::TextColored(r.esSerie ? ImVec4(0,1,0,1) : ImVec4(0.4f,0.7f,1,1), r.esSerie ? "[SERIE]" : "[PELI]");
            ImGui::SameLine(); ImGui::Text("%s (%.1f)", r.titulo.c_str(), r.rating);
            ImGui::SameLine();
            if (ImGui::SmallButton(("+##" + std::to_string(r.id)).c_str())) guardarFavorito(r.id, r.titulo, r.esSerie);
        }
        ImGui::EndChild();
    } else {
        ImGui::Text("Tus Favoritos:"); ImGui::Separator();
        for (auto& f : misFavoritos) {
            ImGui::TextColored(f.esSerie ? ImVec4(0,1,0,1) : ImVec4(0.4f,0.7f,1,1), f.esSerie ? "[SERIE]" : "[PELI]");
            ImGui::SameLine(); ImGui::BulletText("%s", f.titulo.c_str()); ImGui::SameLine();
            if (ImGui::SmallButton(("X##" + std::to_string(f.id)).c_str())) { eliminarFavorito(f.id); refrescarFavoritos(); }
        }
    }
    ImGui::EndGroup();
    ImGui::End();
}

int main() {
    initDB();
    if (!glfwInit()) return 1;
    GLFWwindow* window = glfwCreateWindow(1200, 800, "CineMatch Explorer", NULL, NULL);
    glfwMakeContextCurrent(window);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 130");

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        if (estadoActual == LOGIN) PantallaLogin();
        else if (estadoActual == SELECCION_INICIAL) PantallaSeleccion();
        else PantallaDashboard();
        ImGui::Render();
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    sqlite3_close(db);
    return 0;
}