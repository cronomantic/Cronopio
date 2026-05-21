/* <locale.h> — Cronopio SDK freestanding libc.
 *
 * The console has a single fixed "C" locale. setlocale is a no-op returning
 * "C"; localeconv returns a static struct with a "." decimal point. Enough for
 * ports that probe the locale's decimal separator (e.g. Crispy m_config.c).
 */
#ifndef CVM_LIBC_LOCALE_H
#define CVM_LIBC_LOCALE_H

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#endif /* CVM_LIBC_LOCALE_H */
