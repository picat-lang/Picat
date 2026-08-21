%it works best with cp instead of sat
% run it like this: aspic cp jssp2.lp

%2 2
%3 1 3 2 2 1 2
%3 2 2 1 3 2 1


t(1,1,1,3).
t(1,2,2,2).
t(1,3,1,2).

t(2,1,2,2).
t(2,2,1,3).
t(2,3,2,1).



time(1..8).
j(1..2).
m(1..2).
ta(1..3).


% aux pred, start and duration of task
d(Job,Task,Start,Duration):-j(Job),ta(Task),m(Machine),t(Job,Task,Machine,Duration),s(Job,Task,Start).

% same task
eq(J1,T1,J2,T2):-j(J1),j(J2),ta(T1),ta(T2),J1=J2,T1=T2.

% every task assigned a single start time
1{s(Job,Task,Time):time(Time)}1:-j(Job),ta(Task),m(Machine),t(Job,Task,Machine,Duration).

% job tasks not overlapping
co2(J,T,Time):-j(J),ta(T),d(J,T,S,D),time(Time),Time>=S,Time<S+D.
:-j(J),time(Time),2{co2(J,T,Time):ta(T)}.

% job tasks start in the corect order
:-j(J),ta(T1),ta(T2),s(J,T1,S1),s(J,T2,S2),T1<T2,S1>S2.

% no overlapping tasks on machines
covered(J,T,M,Time):-j(J),ta(T),m(M),d(J,T,S,D),t(J,T,M,D),time(Time),Time>=S,Time<S+D.
:-time(Time),m(M),2{covered(J,T,M,Time):j(J),ta(T)}.

 
{makespan(M):time(M)}=1. % Makespan is one timeslot
% and no task can finish after the makespan
:-makespan(M),j(J),ta(T),d(J,T,S,D),S+D-1>M. 
%and we look for the shortest makespan
#minimize {M:makespan(M)}.

% #show s/3.
