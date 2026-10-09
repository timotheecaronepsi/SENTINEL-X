// ============================================================
//  MODELE de fichier secrets  (celui-ci peut aller sur Git)
//
//  1. Copie ce fichier et renomme la copie en  secrets.h
//  2. Remplis secrets.h avec les vraies valeurs
//  3. Ne mets JAMAIS secrets.h sur Git (il est dans .gitignore)
// ============================================================
#pragma once

// --- Wi-Fi (2,4 GHz uniquement) ---
const char* WIFI_SSID     = "NOM_DU_WIFI";
const char* WIFI_PASSWORD = "MOT_DE_PASSE_WIFI";

// --- Serveur MQTT ---
const char* MQTT_HOST     = "192.168.X.X";
const int   MQTT_PORT     = 1883;          // un nombre, sans guillemets
const char* MQTT_USER     = "UTILISATEUR";
const char* MQTT_PASSWORD = "MOT_DE_PASSE_MQTT";

// Début de l'identifiant MQTT (garder le tiret final :
// l'identifiant unique de la carte est ajouté automatiquement)
const char* MQTT_CLIENT_ID_PREFIX = "PREFIXE-";
