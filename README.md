CINEMATCH.
CineMatch es un motor de curaduría para películas y series desarrollado en C++. La aplicación permite a los usuarios autenticarse con sus credenciales reales de TMDB, gestionar una lista de favoritos persistente mediante SQLite y recibir recomendaciones personalizadas basadas en sus gustos iniciales.
 
 CARACTERÍSTICAS.
•	Autenticación Real: Validación de usuarios mediante la API de The Movie Database (TMDB).
•	Persistencia de Datos: Uso de SQLite para almacenar tus títulos favoritos de forma local.
•	Interfaz Gráfica: Desarrollada con ImGui y OpenGL3 para una experiencia fluida.
•	Multiplataforma: Diseñado para correr en entornos Linux (nativo o WSL).

 REQUISITOS DEL SISTEMA.
1. Dependencias de Software
Para compilar y ejecutar este proyecto, necesitas instalar las siguientes librerías:
Bash
sudo apt update
sudo apt install build-essential libcurl4-openssl-dev libsqlite3-dev libglfw3-dev libglew-dev

2. Configuración de Entorno Gráfico (Importante para WSL)
Si utilizas Windows Subsystem for Linux (WSL), el programa requiere un servidor X activo en Windows para mostrar la ventana:
1.	Instala y ejecuta XLaunch (VcXsrv).
2.	Al configurarlo, selecciona "Multiple windows" y asegúrate de marcar la opción "Disable access control".
3.	En tu terminal de WSL, configura la variable de entorno:

export DISPLAY=$(cat /etc/conf | grep nameserver | awk '{print $2}'):0

 INSTALACIÓN Y COMPILACIÓN.
1.	Clonar el repositorio: en tu terminal de WSL escribe los comandos en el orden siguiente:

    git clone https://github.com/williamsjtorres20/CineMatch.git
    cd CineMatch

2.	Compilar: presiona las teclas:
   CRTL + SHIFT + B (no es necesario tener abierto el terminal para este paso)

3.	Ejecutar: En tu terminal escribe:
    ./CineMatch_app

 Estructura del Proyecto
•	CineMatch.cpp: Código fuente principal con la lógica de la aplicación y la interfaz.
•	include/: Cabeceras del proyecto.
•	external/imgui/: Archivos de la librería gráfica Dear ImGui.
•	cinematch.db: Base de datos local generada automáticamente para los favoritos.

