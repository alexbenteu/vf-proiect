# Runner pentru MiniSat (Proiect SAT 2025)

Ăsta e un script Python care ne face viața mai ușoară.

Pe scurt: ia o grămadă de fișiere `.cnf.xz` (benchmark-uri), aplică `minisat` pe ele pe rând, vede dacă sunt SAT sau UNSAT (sau dacă dă TIMEOUT) și la final scrie un raport frumos cu toate rezultatele.

## De ce ai nevoie ca să meargă

Înainte să-l rulezi:

1.  **Python 3**: Scriptul nu are nevoie de niciun pachet special (`pandas` sau `numpy`).
2.  **MiniSat**: Aceasta e piesa de bază. Scriptul **nu** îl instalează, ci așteaptă să-l găsească în sistem (în `PATH`).