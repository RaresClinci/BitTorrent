# BitTorrent - Tema 2 APD

## Overview
Aplicatia este scrisa in C++, folosind biblioteca MPI.

Are 2 tipuri de procese: tracker si client.

- ```Clientii``` detin un numar de fisiere si doresc descarcarea altor fisiere. Pentru a descarca, un client face cereri altor clienti
- ```Trackerul``` retine ce fisiere detine fiecare client si ce segmente are un fisier(nu ce segmente detine un client!!!), trimitand cele 2 liste clientilor care doresc sa descarce un fisier

## Functii ajutatoare

Pentru o implementare mai usoara, am facut functiile ```mpi_send_string``` si ```mpi_recv_string``` peste functiile MPI_Send si MPI_Recv pentru a trimite mai usor stringuri din C++

Functiile ```assign_order```, ```extract_order``` si ```extract_segment``` sunt facute pentru ca segmentele comunicate intre tracker si client sa ajunga in ordine. Practic, segmentului la trimitere i se ataseaza un numar de ordine de 2 cifre(un fisier are max 100 de segmente conform scheletului).

## Componentele aplicatiei
### Tracker
Variabile:
- ```centralizer``` - un map in care trackerul face corespondenta intre numele unui fisier si informatiile sale(lista hashurilor segmentelor si lista de peers)

Mod de operare:
- ```Initializare```: trackerul primeste informatiile de la clienti si le strange in **centralizer**, apoi trimite clientilor un semnal pentru a termina etapa de initializare
- ```Rulare propriu-zisa```: trackerul raspunde la mesaje dupa tag:
    - ```REQUEST_FILE``` trackerul trimite toate informatile despre un fisier corespondentului si il marcheaza ca peer
    - ```REQUEST_UPDATE``` trackerul trimite lista de peers a fisierului corespondentului
    - ```FILE_DONE``` corespondentul devine seed pentru fisier(nu are o implementare propriu-zisa, programul nefacand distinctia intre peer si seed)
    - ```CLIENT_DONE```: trackerul contorizeaza numarul de clienti care au terminat, cand toti au terminat, semnaleaza upload threadurile sa se opreasca

### Peer
Variabile:
- ```num_files``` - numarul de fisiere detinut initial
- ```segment_list``` - o lista cu segmentele detinute pentru fiecare fisier
- ```segment_mutex``` - mutex care protejeaza **segment_list**
- ```files_needed``` - lista de fisiere pe care clientul vrea sa le descarce

Peer-ul prima oara face pasul de ```initializare```: citeste datele din fisier, le trimite la tracker, apoi asteapta semnalul trackerului pentru finalizarea stadiului de initializare.

Apoi, peerul deschie 2 threaduri: ```download``` - se ocupa cu descarcarea segmenterlor de la alti clienti, ```upload``` - raspunde cererilor de descarcare de la alti clienti.

Implementare detaliata:
- ```download thread```:
    - pentru fiecare fisier, cere informatiile despre fisier de la **tracker** apoi, pentru fiecare segment, cicleaza prin lista de segmente si merge circular prin lista de peers, intreband de un segment, oprindu-se cand unul raspunde **OK** 
    - la 10 segmente descarcate, cere tarckerului sa ii updateze lista de peers
    - pentru a nu ```suprasolicita``` un peer, pozitia de start in ciclul de cereri este generata random, fiecare client are un seed random in functie de rank, pentru a diversifica pozitile de start
    - nu are citiriile din ```segment_list``` protejate de mutex, deoarece doar el scrie
- ```upload_thread```:
    - primeste un nume de fisier si un hash de la un client, raspunde cu **OK** daca il detine si cu **NO** daca nu
    - poate primi **STOP** de la tracker, oprindu-si executia - mesajul de **STOP** este in scris in segment(hashurile sunt in hexa, nu pot sa contina "stop", insa un fisier poate avea numele "stop")
