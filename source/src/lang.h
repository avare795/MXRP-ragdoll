/*
 * lang.h - langue des textes du plugin à l'écran (menu P et messages)
 * Les textes sont écrits en français dans le code ; tr() donne la traduction.
 * (sans accents : la police du jeu ne les a pas toutes)
 */
#ifndef MXB_RAGDOLL_LANG_H
#define MXB_RAGDOLL_LANG_H

enum { LANG_FR = 0, LANG_EN, LANG_ES, LANG_DE, LANG_IT, LANG_COUNT };
/* v1.8.118 : réglage « Auto » : langue de Windows (anglais si ce n'est aucune des 5) */
#define LANG_AUTO LANG_COUNT
/* langue de Windows parmi les 5 */
int lang_system(void);

void lang_set(int lang);
int lang_get(void);
/* nom de la langue (dans cette langue) */
const char *lang_name(int lang);
/* traduction d'un texte français (le texte lui-même s'il n'est pas dans la table) */
const char *tr(const char *fr);

#endif
