# Makefile per opcua_supervisor

# Nome del file sorgente C++ principale (senza estensione)
CXX_SOURCES = opcua_supervisor.cpp

# Aggiungiamo il file sorgente C della libreria open62541
C_SOURCES = open62541.c

# Flag di compilazione per C++ (abilitiamo C++17 per usare std::thread e std::mutex agevolmente)
CXXFLAGS = -std=c++17 -O3 -Wall -D_WIN32 -D_WIN32_WINNT=0x0600 -DUA_ARCHITECTURE_WIN32

# Flag di compilazione per C (necessari per open62541, disabilitiamo i warning della libreria di terze parti per log puliti)
CFLAGS = -O3 -w -D_WIN32 -D_WIN32_WINNT=0x0600 -DUA_ARCHITECTURE_WIN32

# Librerie di sistema necessarie (ws2_32 e crypt32 sono spesso richieste da open62541 su Windows)
LIBRARIES = -lws2_32 -lcrypt32 -liphlpapi

# Includiamo il Makefile standard di Webots
null :=
space := $(null) $(null)
WEBOTS_HOME_PATH=$(subst $(space),\ ,$(strip $(subst \,/,$(WEBOTS_HOME))))
include $(WEBOTS_HOME_PATH)/resources/Makefile.include
