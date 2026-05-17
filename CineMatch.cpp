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
#include <cstdlib>

using json = nlohmann::json;
const std::string API_KEY = "e059ee50571ca68d3761e10bbc575257";

// --- 1. ESTRUCTURA DE DATOS ---
struct Pelicula {
    int id;
    std::string titulo;
    std::string actorPrincipal; // Nueva dimensión para el motor de curaduría
    std::vector<int> generos;
    float rating;
    float puntajeAfinidad;
    bool esSerie;
};

// --- 2. CLASE PERFIL (Maneja los gustos del usuario) ---
class Perfil {
public:
    std::vector<int> generosFavoritos;
    std::vector<std::string> actoresFavoritos;

    void cargarGustosDesdeDB(sqlite3* db) {
        generosFavoritos.clear();
        actoresFavoritos.clear();
        
        sqlite3_stmt* stmt;
        // Seleccionamos géneros y actores de la tabla favoritos
        const char* sql = "SELECT actor, id_genero FROM favoritos_detalles;"; 
        // Nota: Asumimos que guardas los géneros asociados a tus favoritos en una tabla relacional
        
        // Para este ejemplo, simplificamos la carga de actores:
        sqlite3_prepare_v2(db, "SELECT DISTINCT actor FROM favoritos WHERE actor IS NOT NULL;", -1, &stmt, 0);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* act = (const char*)sqlite3_column_text(stmt, 0);
            if (act) actoresFavoritos.push_back(std::string(act));
        }
        sqlite3_finalize(stmt);
    }
};

// --- 3. MOTOR DE CURADURÍA CON HEURÍSTICA DE REPARTO ---
class MotorCuraduria {
public:
    void ejecutarBusquedaAutomatica(Perfil& perfil, std::vector<Pelicula>& catalogo) {
        for (auto& peli : catalogo) {
            float puntos = 0;

            // DIMENSIÓN A: Géneros (Peso base 1.0)
            for (int idGusto : perfil.generosFavoritos) {
                for (int idPeli : peli.generos) {
                    if (idGusto == idPeli) puntos += 1.0f;
                }
            }

            // DIMENSIÓN B: Casting (Peso prioritario 2.5)
            // Si el protagonista de esta peli está en tu lista de actores favoritos
            for (const std::string& actorFav : perfil.actoresFavoritos) {
                if (!peli.actorPrincipal.empty() && peli.actorPrincipal == actorFav) {
                    puntos += 2.5f; 
                }
            }

            peli.puntajeAfinidad = puntos;
        }

        // Ordenamiento por Afinidad > Rating
        std::sort(catalogo.begin(), catalogo.end(), [](const Pelicula& a, const Pelicula& b) {
            if (a.puntajeAfinidad != b.puntajeAfinidad)
                return a.puntajeAfinidad > b.puntajeAfinidad;
            return a.rating > b.rating;
        });
    }
};

// --- 4. EXTRACCIÓN DE DATOS DE LA API (Parsing JSON) ---
std::vector<Pelicula> procesarRespuestaAPI(std::string jsonRaw, bool esSerie) {
    std::vector<Pelicula> lista;
    auto datos = json::parse(jsonRaw);

    for (auto& item : datos["results"]) {
        Pelicula p;
        p.id = item["id"];
        p.titulo = esSerie ? item["name"] : item["title"];
        p.rating = item.value("vote_average", 0.0f);
        p.esSerie = esSerie;
        p.puntajeAfinidad = 0;

        // Extraer géneros
        if (item.contains("genre_ids")) {
            for (auto& g : item["genre_ids"]) p.generos.push_back(g);
        }

        // EXTRAER ACTOR PRINCIPAL
        // TMDB en el endpoint 'discover' no siempre manda el cast. 
        // Si no viene, el programa lo marca como "Pendiente" para una sub-consulta.
        if (item.contains("character_main")) { // Campo hipotético según tu integración
            p.actorPrincipal = item["character_main"];
        } else {
            p.actorPrincipal = "Desconocido"; 
        }

        lista.push_back(p);
    }
    return lista;
}

// --- 5. PERSISTENCIA (SQLite) ---
void guardarEnFavoritos(sqlite3* db, const Pelicula& p) {
    sqlite3_stmt* stmt;
    // IMPORTANTE: Tu tabla debe tener la columna 'actor'
    const char* sql = "INSERT OR REPLACE INTO favoritos (id, titulo, esSerie, actor) VALUES (?, ?, ?, ?);";
    
    sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    sqlite3_bind_int(stmt, 1, p.id);
    sqlite3_bind_text(stmt, 2, p.titulo.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, p.esSerie ? 1 : 0);
    sqlite3_bind_text(stmt, 4, p.actorPrincipal.c_str(), -1, SQLITE_STATIC);
    
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

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
std::string mensajeError = "";
bool cargandoLogin = false;

// --- COMUNICACIÓN CON API (Base) ---
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string peticionPost(std::string url, json body) {
    CURL* curl = curl_easy_init();
    std::string rb;
    if(curl) {
        std::string jsonStr = body.dump();
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    return rb;
}

// --- AUTENTICACIÓN REAL CON TMDB ---
bool validarConTMDB(std::string usuario, std::string pass) {
    try {
        // 1. Obtener Token temporal
        std::string urlToken = "https://api.themoviedb.org/3/authentication/token/new?api_key=" + API_KEY;
        CURL* curl = curl_easy_init();
        std::string resToken;
        curl_easy_setopt(curl, CURLOPT_URL, urlToken.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resToken);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        auto jToken = json::parse(resToken);
        if(!jToken.value("success", false)) return false;
        std::string token = jToken["request_token"];

        // 2. Validar credenciales con ese token
        std::string urlAuth = "https://api.themoviedb.org/3/authentication/token/validate_with_login?api_key=" + API_KEY;
        json body = {
            {"username", usuario},
            {"password", pass},
            {"request_token", token}
        };
        
        std::string resAuth = peticionPost(urlAuth, body);
        auto jAuth = json::parse(resAuth);
        
        return jAuth.value("success", false);
    } catch (...) {
        return false;
    }
}

// --- GESTIÓN DE DATOS Y URLS ---
void abrirURL(const char* url) {
    #if defined(_WIN32) || defined(_WIN64)
        std::string comando = "start " + std::string(url);
        system(comando.c_str());
    #else
        std::string cmdWSL = "powershell.exe -Command Start-Process '" + std::string(url) + "' > /dev/null 2>&1";
        if (system(cmdWSL.c_str()) != 0) system(("xdg-open " + std::string(url) + " > /dev/null 2>&1").c_str());
    #endif
}

void initDB() {
    sqlite3_open("cinematch.db", &db);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS favoritos(id INTEGER PRIMARY KEY, titulo TEXT, esSerie INTEGER);", 0, 0, 0);
}

void refrescarFavoritos() {
    misFavoritos.clear();
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT id, titulo, esSerie FROM favoritos;", -1, &stmt, 0);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        misFavoritos.push_back({sqlite3_column_int(stmt, 0), (const char*)sqlite3_column_text(stmt, 1), 0.0f, sqlite3_column_int(stmt, 2) == 1});
    }
    sqlite3_finalize(stmt);
}

std::vector<Resultado> pedirAPI(std::string url, bool esSerie) {
    CURL* curl = curl_easy_init();
    std::string rb; std::vector<Resultado> res;
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        if(curl_easy_perform(curl) == CURLE_OK) {
            auto datos = json::parse(rb);
            for(auto& item : datos["results"]) {
                res.push_back({item["id"], item.contains("title") ? item["title"] : item["name"], item.value("vote_average", 0.0f), esSerie});
            }
        }
        curl_easy_cleanup(curl);
    }
    return res;
}

void cargarSeleccionMasiva() {
    opcionesSeleccion.clear();
    // Pedimos 2 páginas de películas y 2 de series para tener variedad balanceada
    for(int i = 1; i <= 2; i++) {
        // Cargar Películas
        std::string urlMovies = "https://api.themoviedb.org/3/movie/top_rated?api_key=" + API_KEY + 
                                "&language=es-ES&page=" + std::to_string(i);
        auto movies = pedirAPI(urlMovies, false);
        opcionesSeleccion.insert(opcionesSeleccion.end(), movies.begin(), movies.end());

        // Cargar Series
        std::string urlTV = "https://api.themoviedb.org/3/tv/top_rated?api_key=" + API_KEY + 
                            "&language=es-ES&page=" + std::to_string(i);
        auto series = pedirAPI(urlTV, true);
        opcionesSeleccion.insert(opcionesSeleccion.end(), series.begin(), series.end());
    }
    // Mezclamos para que no salgan todas las pelis juntas y luego las series
    std::shuffle(opcionesSeleccion.begin(), opcionesSeleccion.end(), std::mt19937(std::random_device()()));
}

void cargarDashboard(VistaDashboard vista) {
    resultadosPantalla.clear();
    std::string base = "https://api.themoviedb.org/3/discover/";
    std::string params = "?api_key="+API_KEY+"&vote_average.gte=7.0&language=es-ES&sort_by=popularity.desc";
    if(vista != SOLO_SERIES) { auto p = pedirAPI(base+"movie"+params, false); resultadosPantalla.insert(resultadosPantalla.end(), p.begin(), p.end()); }
    if(vista != SOLO_PELIS) { auto s = pedirAPI(base+"tv"+params, true); resultadosPantalla.insert(resultadosPantalla.end(), s.begin(), s.end()); }
    std::shuffle(resultadosPantalla.begin(), resultadosPantalla.end(), std::mt19937(std::random_device()()));
}

// --- INTERFAZ ---
void PantallaLogin() {
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Login", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    float sw = ImGui::GetIO().DisplaySize.x, sh = ImGui::GetIO().DisplaySize.y;

    ImGui::SetCursorPosY(sh * 0.25f); ImGui::SetWindowFontScale(4.0f);
    float tw = ImGui::CalcTextSize("CINEMATCH").x;
    ImGui::SetCursorPosX((sw - tw) * 0.5f);
    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.1f, 1.0f), "CINEMATCH"); 
    ImGui::SetWindowFontScale(1.0f);

    static char u[64] = "", p[64] = "";
    ImGui::SetCursorPosY(sh * 0.45f);
    ImGui::SetCursorPosX((sw - 250) * 0.5f); ImGui::SetNextItemWidth(250);
    ImGui::InputTextWithHint("##u", "Usuario TMDB", u, 64);
    ImGui::SetCursorPosX((sw - 250) * 0.5f); ImGui::SetNextItemWidth(250);
    ImGui::InputTextWithHint("##p", "Contraseña", p, 64, ImGuiInputTextFlags_Password);

    if (!mensajeError.empty()) {
        ImGui::SetCursorPosX((sw - ImGui::CalcTextSize(mensajeError.c_str()).x) * 0.5f);
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", mensajeError.c_str());
    }

    ImGui::SetCursorPosY(sh * 0.62f);
    ImGui::SetCursorPosX((sw - 120) * 0.5f);
    if (cargandoLogin) ImGui::Text("Verificando...");
    else if (ImGui::Button("ENTRAR", ImVec2(120, 40))) {
        cargandoLogin = true;
        if (validarConTMDB(u, p)) {
            mensajeError = "";
            sqlite3_stmt* s; sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM favoritos;", -1, &s, 0);
            int n = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int(s, 0) : 0;
            sqlite3_finalize(s);
            if (n >= 5) { cargarDashboard(MEZCLA); refrescarFavoritos(); estadoActual = DASHBOARD; }
            else { cargarSeleccionMasiva(); estadoActual = SELECCION_INICIAL; }
        } else {
            mensajeError = "Usuario o contraseña incorrectos en TMDB.";
        }
        cargandoLogin = false;
    }

    ImGui::SetCursorPosY(sh * 0.75f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.6f));
    const char* t1 = "¿Olvidaste tu contraseña?"; float w1 = ImGui::CalcTextSize(t1).x;
    ImGui::SetCursorPosX((sw - w1) * 0.5f); if (ImGui::Selectable(t1, false, 0, ImVec2(w1,0))) abrirURL("https://www.themoviedb.org/reset-password");
    const char* t2 = "Regístrate en TMDB"; float w2 = ImGui::CalcTextSize(t2).x;
    ImGui::SetCursorPosX((sw - w2) * 0.5f); if (ImGui::Selectable(t2, false, 0, ImVec2(w2,0))) abrirURL("https://www.themoviedb.org/signup");
    ImGui::PopStyleColor();
    ImGui::End();
}

void PantallaSeleccion() {
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Seleccion", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::Text("Personaliza tu perfil eligiendo al menos 5 títulos (Películas y Series):");
    ImGui::Separator();

    ImGui::BeginChild("ScrollRegion", ImVec2(0, -50), true);
    for (auto& res : opcionesSeleccion) {
        // Dibujar Etiqueta de Tipo
        if (res.esSerie) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f)); // Verde para Series
            ImGui::Text("[SERIE]");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.6f, 1.0f, 1.0f)); // Azul para Películas
            ImGui::Text("[PELÍCULA]");
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();

        bool is_selected = (elegidas.find(res.id) != elegidas.end());
        if (ImGui::Selectable((res.titulo + "##" + std::to_string(res.id)).c_str(), is_selected)) {
            if (is_selected) elegidas.erase(res.id);
            else elegidas.insert(res.id);
        }
    }
    ImGui::EndChild();

    ImGui::Text("Seleccionados: %zu/5", elegidas.size());
    ImGui::SameLine();
    if (elegidas.size() >= 5) {
        if (ImGui::Button("CONTINUAR")) {
            for(int id : elegidas) {
                for(auto& o : opcionesSeleccion) {
                    if(o.id == id) {
                        sqlite3_stmt* st; sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO favoritos VALUES (?, ?, ?);", -1, &st, 0);
                        sqlite3_bind_int(st, 1, o.id); sqlite3_bind_text(st, 2, o.titulo.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(st, 3, o.esSerie ? 1 : 0);
                        sqlite3_step(st); sqlite3_finalize(st);
                    }
                }
            }
            cargarDashboard(MEZCLA);
            refrescarFavoritos();
            estadoActual = DASHBOARD;
        }
    }
    ImGui::End();
}

void PantallaDashboard() {
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Dash", nullptr, ImGuiWindowFlags_NoTitleBar);
    
    // Panel Lateral de Navegación
    ImGui::BeginChild("Side", ImVec2(200, 0), true);
    if (ImGui::Selectable("Recomendaciones", !viendoFavoritos)) { viendoFavoritos = false; }
    if (ImGui::Selectable("Mis Favoritos", viendoFavoritos)) { viendoFavoritos = true; refrescarFavoritos(); }
    ImGui::EndChild(); 
    
    ImGui::SameLine();
    
    // Panel Central de Contenido
    ImGui::BeginChild("Cont");
    if (viendoFavoritos) {
        for (auto& f : misFavoritos) {
            // Etiquetas también en favoritos para mantener consistencia visual
            if (f.esSerie) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));
                ImGui::Text("[SERIE]");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
                ImGui::Text("[PELÍCULA]");
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            
            ImGui::Text("%s", f.titulo.c_str()); ImGui::SameLine();
            if (ImGui::SmallButton(("Eliminar##"+std::to_string(f.id)).c_str())) {
                sqlite3_exec(db, ("DELETE FROM favoritos WHERE id="+std::to_string(f.id)).c_str(), 0, 0, 0);
                refrescarFavoritos();
            }
        }
    } else {
        // --- LISTA DE RECOMENDACIONES CON ETIQUETAS ---
        for (auto& r : resultadosPantalla) {
            if (r.esSerie) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f)); // Verde
                ImGui::Text("[SERIE]");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.6f, 1.0f, 1.0f)); // Azul
                ImGui::Text("[PELÍCULA]");
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            ImGui::Text("%s (%.1f)", r.titulo.c_str(), r.rating); ImGui::SameLine();
            if (ImGui::SmallButton(("+##"+std::to_string(r.id)).c_str())) {
                sqlite3_stmt* st; sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO favoritos VALUES (?, ?, ?);", -1, &st, 0);
                sqlite3_bind_int(st, 1, r.id); 
                sqlite3_bind_text(st, 2, r.titulo.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int(st, 3, r.esSerie ? 1 : 0); 
                sqlite3_step(st); sqlite3_finalize(st);
            }
        }
    }
    ImGui::EndChild(); 
    ImGui::End();
}

int main() {
    initDB(); curl_global_init(CURL_GLOBAL_ALL);
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
        ImGui::Render(); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    sqlite3_close(db);
    return 0;
}