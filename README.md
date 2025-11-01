# Runner pentru MiniSat (Proiect SAT 2025)

Un script Python care ne face viața mai ușoară.

Pe scurt: ia fișiere `.cnf.xz` (benchmark-uri), aplică `minisat`, verifică dacă sunt SAT sau UNSAT (sau dacă dau TIMEOUT) și la final scrie un raport cu toate rezultatele.

Înainte să-l rulezi:

1.  **Python 3**: Scriptul nu are nevoie de niciun pachet special (`pandas` sau `numpy`).
2.  **MiniSat**: Aceasta e piesa de bază. Scriptul **nu** îl instalează, ci așteaptă să-l găsească în sistem (în `PATH`).
