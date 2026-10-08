#pragma once
// Modèle (nœud radar esp02) : copier ce fichier en "secrets.h" dans le même dossier, puis le remplir.
// secrets.h est dans le .gitignore : aucun secret dans le dépôt.

#define WIFI_SSID   "SENTINEL-TABLE"   // réseau Wi-Fi de la table (2,4 GHz)
#define WIFI_PASS   "changeme"

#define MQTT_HOST   "192.168.10.1"     // broker Mosquitto du PC portable de la table
#define MQTT_PORT   1883               // 8883 quand le TLS sera en place
#define MQTT_USER   "esp02"
#define MQTT_PASS   "changeme"

#define NTP_SERVER  "192.168.10.1"     // ou "pool.ntp.org" si la table a internet

#define TABLE_ID    "t1"
#define NODE_ID     "esp02"
