// nfs5-web ON2: career tournaments online (docs/research/ON2-career-tournaments.md).
//
// The server owns the tournament: which event, the next race, the grid (the event's own opponents as bots), the points
// of every slot and the standings, by the game's own rules (00503960 points = 9 - place, 00503660 / 00502880 order:
// points desc, money desc, time asc). The game's creator gets each race's track and laps over the control channel
// (UDP 9443 on the virtual network, runtime/web/winsock_web.c online_ctl_*, runtime/nettest.c online_auto) and reports
// the finishing order of all cars after the race. Standings go to the game's chat.
// Stage 2 (not here yet): the clients apply the results to their own career with the stock code (ON2 section 4.1).
import fs from 'node:fs';

const REC = 0xD40, OPP = 0x58, RACE = 0xA4;
// nfs5.trk id -> the track list's English name (festrings 300..315)
export const TRACK_NAMES = {
  8: "C\xf4te d'Azur", 9: 'Schwarzwald', 10: 'Pyr\xe9n\xe9es', 11: 'Alps', 12: 'Autobahn', 13: 'Auvergne', 14: 'Normandie',
  16: 'Zone Industrielle', 18: 'Monte Carlo Circuit 1', 25: 'Monte Carlo Circuit 2', 26: 'Monte Carlo Circuit 3',
  29: 'Monte Carlo Circuit 4', 30: 'Monte Carlo Circuit 5', 32: 'Skid Pad', 34: 'Corsica',
};
const GROUPS = { 0x209: 'Classic era', 0x20a: 'Golden era', 0x20b: 'Modern era', 0x20c: 'Classic club', 0x20d: 'Golden club', 0x20e: 'Modern club' };

// nfs5.trn: 35 records of 0xD40 (ON2 section 1)
export function loadTournaments(file) {
  if (!file || !fs.existsSync(file)) return [];
  const d = fs.readFileSync(file), list = [];
  for (let o = 0; o + REC <= d.length; o += REC) {
    const t = {
      index: list.length, id: d.readInt32LE(o), group: d.readInt32LE(o + 4), year: d.readInt16LE(o + 8),
      nameId: d.readInt32LE(o + 0x24), fee: d.readInt32LE(o + 0xDC), traffic: d.readInt16LE(o + 0xE0),
      club: d.readInt16LE(o + 0xE2), hidden: d.readInt16LE(o + 0xE4), difficulty: d.readInt16LE(o + 0xE6),
      rally: d[o + 0xE8], finalPrize: [...Array(8)].map((_, p) => d.readInt32LE(o + 0xEC + p * 4)),
      trafficList: d[o + 0x380], opponents: [], races: [],
    };
    const nopp = Math.min(7, d.readInt16LE(o + 0x114));
    for (let k = 0; k < nopp; k++) {
      const p = o + 0x118 + k * OPP;
      t.opponents.push({ personality: d.readInt16LE(p), car: d.readInt16LE(p + 2), colour: d.readInt32LE(p + 0x34) });
    }
    const nr = Math.min(10, d.readInt16LE(o + 0x39C));
    for (let r = 0; r < nr; r++) {
      const p = o + 0x3A0 + r * RACE;
      t.races.push({ track: d.readInt16LE(p), laps: Math.max(1, d.readInt16LE(p + 2)), reverse: d[p + 4], mirror: d[p + 5],
                     prize: [...Array(8)].map((_, k) => d.readInt32LE(p + 8 + k * 4)), bonus: d.readInt32LE(p + 0x28),
                     skill: d.readFloatLE(p + 0x2C) });
    }
    t.groupName = GROUPS[t.group] || `group ${t.group}`;
    t.title = `${t.groupName} #${t.id}: ` + t.races.map(r => TRACK_NAMES[r.track] || `track ${r.track}`).join(', ');
    list.push(t);
  }
  return list;
}

// Online events: visible ones without traffic (network races set no traffic; ON2 section 3) and with known tracks
export function onlineEvents(list) {
  return list.filter(t => !t.hidden && !t.traffic && !t.trafficList && t.races.length && t.races.every(r => TRACK_NAMES[r.track]));
}

// One tournament run by a game. Entrants are fixed at the first race: slot -> { name, bot }.
export function createTournament(def, log) {
  const T = { def, race: 0, entrants: new Map(), results: [], done: false };
  T.current = () => def.races[T.race];
  T.directive = () => {
    const r = T.current();
    return r ? `race ${T.race + 1}/${def.races.length} laps=${r.laps} reverse=${r.reverse} track=${TRACK_NAMES[r.track]}` : 'over';
  };
  T.enter = (slot, name, bot) => { if (!T.entrants.has(slot)) T.entrants.set(slot, { name, bot, points: 0, money: 0, time: 0 }); };
  // places: slot -> place 1..8 (absent = did not finish: no points), times: slot -> 1/64 s
  T.finish = (places, times = new Map()) => {
    const r = T.current();
    if (!r) return null;
    const row = new Map();
    for (const [slot, e] of T.entrants) {
      const place = places.get(slot) || 0;
      const points = place ? 9 - place : 0;
      const prize = place ? r.prize[place - 1] || 0 : 0;
      const bonus = place === 1 ? r.bonus : place === 2 ? r.bonus / 2 : place === 3 ? r.bonus / 4 : 0;
      e.points += points; e.money += prize + bonus; e.time += times.get(slot) || 0;
      row.set(slot, { place, points, prize: prize + bonus });
    }
    T.results.push(row);
    T.race++;
    if (T.race >= def.races.length) {
      T.done = true;
      T.standings().forEach((s, i) => { s.e.money += def.finalPrize[i] || 0; s.final = def.finalPrize[i] || 0; });
    }
    log(`  tournament ${def.title}: race ${T.race} done`);
    return row;
  };
  T.standings = () => [...T.entrants.entries()].map(([slot, e]) => ({ slot, e }))
    .sort((a, b) => def.rally ? (a.e.time - b.e.time || b.e.money - a.e.money)
                              : (b.e.points - a.e.points || b.e.money - a.e.money || a.e.time - b.e.time));
  T.table = () => T.standings().map((s, i) => `${i + 1}. ${s.e.name} ${s.e.points} pts${s.final ? `, prize ${s.final} DM` : ''}`);
  return T;
}
