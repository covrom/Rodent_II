/*
Rodent, a UCI chess playing engine derived from Sungorus 1.4
Copyright (C) 2009-2011 Pablo Vazquez (Sungorus author)
Copyright (C) 2011-2016 Pawel Koziol

Rodent is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published
by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version.

Rodent is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty
of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "rodent.h"
#include "eval.h"

#define SCALE(x,y) ((x*y)/100)

int danger[512];   // table for evaluating king safety
int dist[64][64];  // table for evaluating king tropism

// parameters for defining game phase [6]

static const int max_phase = 24;
const int phase_value[7] = { 0, 1, 1, 2, 4, 0, 0 };

U64 support_mask[2][64];
int mg_pst_data[2][6][64];
int eg_pst_data[2][6][64];
int sp_pst_data[2][6][64];

char *factor_name[] = { "Attack    ", "Mobility  ", "Pst       ", "Pawns     ", "Passers   ", "Tropism   ", "Outposts  ", "Lines     ", "Pressure  ", "Others    "};

sEvalHashEntry EvalTT[EVAL_HASH_SIZE];

void SetAsymmetricEval(int sd) {

  int op = Opp(sd);
  Eval.prog_side = sd;

  curr_weights[sd][SD_ATT] = dyn_weights[DF_OWN_ATT];
  curr_weights[op][SD_ATT] = dyn_weights[DF_OPP_ATT];
  curr_weights[sd][SD_MOB] = dyn_weights[DF_OWN_MOB];
  curr_weights[op][SD_MOB] = dyn_weights[DF_OPP_MOB];
}

void ClearEvalHash(void) {

  for (int e = 0; e < EVAL_HASH_SIZE; e++) {
    EvalTT[e].key = 0;
    EvalTT[e].score = 0;
  }
}

void InitWeights(void) {

  // default weights: 100%
  
  for (int fc = 0; fc < N_OF_FACTORS; fc++)
    weights[fc] = 100;

  weights[F_TROPISM] = 20;
  mat_perc = 100;
  pst_perc = 100;

  // weights for asymmetric factors

  dyn_weights[DF_OWN_ATT] = 110;
  dyn_weights[DF_OPP_ATT] = 100;
  dyn_weights[DF_OWN_MOB] = 100;
  dyn_weights[DF_OPP_MOB] = 110;
}

const int pawnAdv[8] = { 0, 1, 1, 3, 5, 8, 12, 0 };

int GetPhalanxPst(int sq) {

  if (sq == D4) return 15;             // D4/E4 pawns
  if (sq == D3) return 10;             // D3/E3 pawns
  if (sq == C4 || sq == E4) return 10; // C4/D4 or E4/F4 pawns
  return pawnAdv[Rank(sq)] * 2;        // generic bonus for advanced phalanxes
}

int GetDefendedPst(int sq) {
  return pawnAdv[Rank(sq)];
}

void InitEval(void) {

  Eval.prog_side = NO_CL;

  // Init piece/square values together with material value of the pieces.

  for (int sq = 0; sq < 64; sq++) {
    for (int sd = 0; sd < 2; sd++) {
 
      mg_pst_data[sd][P][REL_SQ(sq, sd)] = SCALE(pc_value[P], mat_perc) + SCALE(pstPawnMg[sq], pst_perc);
      eg_pst_data[sd][P][REL_SQ(sq, sd)] = SCALE(pc_value[P], mat_perc) + SCALE(pstPawnEg[sq], pst_perc);
      mg_pst_data[sd][N][REL_SQ(sq, sd)] = SCALE(pc_value[N], mat_perc) + SCALE(pstKnightMg[sq], pst_perc);
      eg_pst_data[sd][N][REL_SQ(sq, sd)] = SCALE(pc_value[N], mat_perc) + SCALE(pstKnightEg[sq], pst_perc);
      mg_pst_data[sd][B][REL_SQ(sq, sd)] = SCALE(pc_value[B], mat_perc) + SCALE(pstBishopMg[sq], pst_perc);
      eg_pst_data[sd][B][REL_SQ(sq, sd)] = SCALE(pc_value[B], mat_perc) + SCALE(pstBishopEg[sq], pst_perc);
      mg_pst_data[sd][R][REL_SQ(sq, sd)] = SCALE(pc_value[R], mat_perc) + SCALE(pstRookMg[sq], pst_perc);
      eg_pst_data[sd][R][REL_SQ(sq, sd)] = SCALE(pc_value[R], mat_perc) + SCALE(pstRookEg[sq], pst_perc);
      mg_pst_data[sd][Q][REL_SQ(sq, sd)] = SCALE(pc_value[Q], mat_perc) + SCALE(pstQueenMg[sq], pst_perc);
      eg_pst_data[sd][Q][REL_SQ(sq, sd)] = SCALE(pc_value[Q], mat_perc) + SCALE(pstQueenEg[sq], pst_perc);
      mg_pst_data[sd][K][REL_SQ(sq, sd)] = pstKingMg[sq];
      eg_pst_data[sd][K][REL_SQ(sq, sd)] = pstKingEg[sq];

      phalanx_data[sd][REL_SQ(sq, sd)] = GetPhalanxPst(sq);
      defended_data[sd][REL_SQ(sq, sd)] = GetDefendedPst(sq);

      sp_pst_data[sd][N][REL_SQ(sq, sd)] = pstKnightOutpost[sq];
      sp_pst_data[sd][B][REL_SQ(sq, sd)] = pstBishopOutpost[sq];
    }
  }

  // Init king attack table

  for (int t = 0, i = 1; i < 512; ++i) {
    t = Min(maxAttScore, Min(int(attCurveMult * i * i), t + maxAttStep));
    danger[i] = (t * 100) / 256; // rescale to centipawns
  }

  // Init king zone

  for (int i = 0; i < 64; i++) {
    bbKingZone[WC][i] = bbKingZone[BC][i] = BB.KingAttacks(i);
    bbKingZone[WC][i] |= ShiftSouth(bbKingZone[WC][i]);
    bbKingZone[BC][i] |= ShiftNorth(bbKingZone[BC][i]);
  }

  // Init mask for passed pawn detection

  for (int sq = 0; sq < 64; sq++) {
    passed_mask[WC][sq] = BB.FillNorthExcl(SqBb(sq));
    passed_mask[WC][sq] |= ShiftWest(passed_mask[WC][sq]);
    passed_mask[WC][sq] |= ShiftEast(passed_mask[WC][sq]);
    passed_mask[BC][sq] = BB.FillSouthExcl(SqBb(sq));
    passed_mask[BC][sq] |= ShiftWest(passed_mask[BC][sq]);
    passed_mask[BC][sq] |= ShiftEast(passed_mask[BC][sq]);
  }

  // Init adjacent mask (for detecting isolated pawns)

  for (int i = 0; i < 8; i++) {
    adjacent_mask[i] = 0;
    if (i > 0) adjacent_mask[i] |= FILE_A_BB << (i - 1);
    if (i < 7) adjacent_mask[i] |= FILE_A_BB << (i + 1);
  }

  // Init support mask (for detecting weak pawns)

  for (int sq = 0; sq < 64; sq++) {
    support_mask[WC][sq] = ShiftWest(SqBb(sq)) | ShiftEast(SqBb(sq));
    support_mask[WC][sq] |= BB.FillSouth(support_mask[WC][sq]);

    support_mask[BC][sq] = ShiftWest(SqBb(sq)) | ShiftEast(SqBb(sq));
    support_mask[BC][sq] |= BB.FillNorth(support_mask[BC][sq]);
  }

  // Init distance table (for evaluating king tropism)

  for (int i = 0; i < 64; ++i) {
    for (int j = 0; j < 64; ++j) {
      dist[i][j] = 14 - (Abs(Rank(i) - Rank(j)) + Abs(File(i) - File(j)));
	  // TODO: init Chebyshev distance here
    }
  }
}

void cEval::ScorePieces(POS *p, int sd) {

  U64 bbPieces, bbMob, bbAtt, bbFile, bbContact;
  int op, sq, cnt, tmp, ksq, att = 0, wood = 0;
  int own_pawn_cnt, opp_pawn_cnt;
  int r_on_7th = 0;

  // Is color OK?

  assert(sd == WC || sd == BC);

  // Init variables

  op = Opp(sd);
  ksq = KingSq(p, op);

  // Init enemy king zone for attack evaluation. We mark squares where the king
  // can move plus two or three more squares facing enemy position.

  U64 bbZone = bbKingZone[sd][ksq];

  // Init bitboards to detect check threats
  
  U64 bbKnightChk = BB.KnightAttacks(ksq);
  U64 bbStr8Chk = BB.RookAttacks(OccBb(p), ksq);
  U64 bbDiagChk = BB.BishAttacks(OccBb(p), ksq);
  U64 bbQueenChk = bbStr8Chk | bbDiagChk;

  // Piece configurations

  tmp = np_bonus * adj[p->cnt[sd][P]] * p->cnt[sd][N]   // knights lose value as pawns disappear
      - rp_malus * adj[p->cnt[sd][P]] * p->cnt[sd][R];  // rooks gain value as pawns disappear

  if (p->cnt[sd][N] > 1) tmp -= 10;                     // Knight pair
  if (p->cnt[sd][R] > 1) tmp -= 5;                      // Rook pair
  if (p->cnt[sd][Q]) 
    tmp -= minorVsQueen * (p->cnt[op][N] + p->cnt[op][B]); // "elephantiasis correction", idea by H.G.Mueller
  
  if (p->cnt[sd][B] > 1) 
     Add(sd, F_OTHERS, SCALE(50,mat_perc),  SCALE(60,mat_perc));  // Bishop pair

  Add(sd, F_OTHERS, SCALE(tmp, mat_perc), SCALE(tmp, mat_perc));
  
  // Knight

  bbPieces = p->Knights(sd);
  while (bbPieces) {
    sq = BB.PopFirstBit(&bbPieces);

    // Knight tropism to enemy king

    Add(sd, F_TROPISM, tropism_mg[N] * dist[sq][ksq], tropism_eg[N] * dist[sq][ksq]);
    
    // Knight mobility

    bbMob = BB.KnightAttacks(sq) & ~p->cl_bb[sd];
    cnt = BB.PopCnt(bbMob &~bbPawnTakes[op]);
    
    Add(sd, F_MOB, n_mob_mg[cnt], n_mob_eg[cnt]);  // mobility bonus

    if ((bbMob &~bbPawnTakes[op]) & bbKnightChk) 
       att += chk_threat[N];                       // check threat bonus

    bbAllAttacks[sd] |= bbMob;
    bbMinorAttacks[sd] |= bbMob;

    // Knight attacks on enemy king zone

	bbAtt = BB.KnightAttacks(sq);
    if (bbAtt & bbZone) {
      wood++;
      att += king_att[N] * BB.PopCnt(bbAtt & bbZone);
    }

    // Knight outpost

	ScoreOutpost(sd, N, sq);

    // Pawn in front of a knight

    if (SqBb(sq) & bbHomeZone[sd]) {
      U64 bbStop = ShiftFwd(SqBb(sq), sd);
      if (bbStop & PcBb(p, sd, P))
        Add(sd, F_OUTPOST, minorBehindPawn, minorBehindPawn);
    }

  } // end of knight eval

  // Bishop

  bbPieces = p->Bishops(sd);
  while (bbPieces) {
    sq = BB.PopFirstBit(&bbPieces);

  // Bishop tropism to enemy king

  Add(sd, F_TROPISM, tropism_mg[B] * dist[sq][ksq], tropism_eg[B] * dist[sq][ksq]);

    // Bishop mobility

    bbMob = BB.BishAttacks(OccBb(p), sq);

    if (!(bbMob & bbAwayZone[sd]))                     // penalty for bishops unable to reach enemy half of the board
       Add(sd, F_MOB, bishConfinedMg, bishConfinedEg); // (idea from Andscacs)

    cnt = BB.PopCnt(bbMob &~bbPawnTakes[op]);
    
    Add(sd, F_MOB, b_mob_mg[cnt], b_mob_eg[cnt]);      // mobility bonus

    if ((bbMob &~bbPawnTakes[op]) & bbDiagChk) 
      att += chk_threat[B];                            // check threat bonus

    bbAllAttacks[sd] |= bbMob;
    bbMinorAttacks[sd] |= bbMob;

    // Bishop attacks on enemy king zone

    bbAtt = BB.BishAttacks(OccBb(p) ^ p->Queens(sd) , sq);
    if (bbAtt & bbZone) {
      wood++;
      att += king_att[B] * BB.PopCnt(bbAtt & bbZone);
    }

    // Bishop outpost

    ScoreOutpost(sd, B, sq);

	// Pawn in front of a bishop

    if (SqBb(sq) & bbHomeZone[sd]) {
      U64 bbStop = ShiftFwd(SqBb(sq), sd);
      if (bbStop & PcBb(p, sd, P))
        Add(sd, F_OUTPOST, minorBehindPawn, minorBehindPawn);
    }

    // Pawns on the same square color as our bishop
  
    if (bbWhiteSq & SqBb(sq)) {
      own_pawn_cnt = BB.PopCnt(bbWhiteSq & p->Pawns(sd)) - 4;
      opp_pawn_cnt = BB.PopCnt(bbWhiteSq & p->Pawns(op)) - 4;
    } else {
      own_pawn_cnt = BB.PopCnt(bbBlackSq & p->Pawns(sd)) - 4;
      opp_pawn_cnt = BB.PopCnt(bbBlackSq & p->Pawns(op)) - 4;
    }

    Add(sd, F_OTHERS, -3 * own_pawn_cnt - opp_pawn_cnt, 
                      -3 * own_pawn_cnt - opp_pawn_cnt);

    // TODO: bishop blocked by defended enemy pawns

  } // end of bishop eval

  // Rook

  bbPieces = p->Rooks(sd);
  while (bbPieces) {
    sq = BB.PopFirstBit(&bbPieces);

    // Rook tropism to enemy king

    Add(sd, F_TROPISM, tropism_mg[R] * dist[sq][ksq], tropism_eg[R] * dist[sq][ksq]);
  
    // Rook mobility

    bbMob = BB.RookAttacks(OccBb(p), sq);
    cnt = BB.PopCnt(bbMob);
    Add(sd, F_MOB, r_mob_mg[cnt], r_mob_eg[cnt]);        // mobility bonus
    if (((bbMob &~bbPawnTakes[op]) & bbStr8Chk)          // check threat bonus
    && p->cnt[sd][Q]) {
      att += chk_threat[R]; 

      bbContact = (bbMob & BB.KingAttacks(ksq)) & bbStr8Chk;

      while (bbContact) {
        int contactSq = BB.PopFirstBit(&bbContact);

        // rook exchanges are accepted as contact checks

        if (Swap(p, sq, contactSq) >= 0) {
          att += r_contact_check;
          break;
        }
      }
    }

    bbAllAttacks[sd] |= bbMob;
    bbMinorAttacks[sd] |= bbMob;  // TEMPORARY

    // Rook attacks on enemy king zone

    bbAtt = BB.RookAttacks(OccBb(p) ^ p->StraightMovers(sd), sq);
    if (bbAtt & bbZone) {
      wood++;
      att += king_att[R] * BB.PopCnt(bbAtt & bbZone);
    }

    // Get rook file

    bbFile = BB.FillNorthSq(sq) | BB.FillSouthSq(sq); // better this way than using front span

    // Queen on rook's file (which might be closed)

    if (bbFile & p->Queens(op)) Add(sd, F_LINES, rookOnQueenMg, rookOnQueenEg);

    // Rook on (half) open file

    if (!(bbFile & p->Pawns(sd))) {
      if (!(bbFile & p->Pawns(op))) Add(sd, F_LINES, rookOnOpenMg, rookOnOpenEg);
      else {
		// score differs depending on whether half-open file is blocked by defended enemy pawn
        if ((bbFile & p->Pawns(op)) & bbPawnTakes[op])
          Add(sd, F_LINES, rookOnBadHalfOpenMg, rookOnBadHalfOpenEg);
        else
          Add(sd, F_LINES, rookOnGoodHalfOpenMg, rookOnGoodHalfOpenEg);
      }
    }

    // Rook on the 7th rank attacking pawns or cutting off enemy king

    if (SqBb(sq) & bbRelRank[sd][RANK_7]) {
      if (p->Pawns(op) & bbRelRank[sd][RANK_7]
      ||  p->Kings(op) & bbRelRank[sd][RANK_8]) {
        Add(sd, F_LINES, rookOnSeventhMg, rookOnSeventhEg);
        r_on_7th++;
      }
    }

  } // end of rook eval

  // Queen

  bbPieces = p->Queens(sd);
  while (bbPieces) {
    sq = BB.PopFirstBit(&bbPieces);

    // Queen tropism to enemy king

    Add(sd, F_TROPISM, tropism_mg[Q] * dist[sq][ksq], tropism_eg[Q] * dist[sq][ksq]);

    // Queen mobility

    bbMob = BB.QueenAttacks(OccBb(p), sq);
    cnt = BB.PopCnt(bbMob);
    Add(sd, F_MOB, q_mob_mg[cnt], q_mob_eg[cnt]);  // mobility bonus

    if ((bbMob &~bbPawnTakes[op]) & bbQueenChk) {  // check threat bonus
      att += chk_threat[Q];

    // Queen contact checks

    bbContact = bbMob & BB.KingAttacks(ksq);
    while (bbContact) {
      int contactSq = BB.PopFirstBit(&bbContact);

        // queen exchanges are accepted as contact checks
	
        if (Swap(p, sq, contactSq) >= 0) {
          att += q_contact_check;
          break;
        }
      }
    }

    bbAllAttacks[sd] |= bbMob;

    // Queen attacks on enemy king zone
   
    bbAtt  = BB.BishAttacks(OccBb(p) ^ p->DiagMovers(sd), sq);
    bbAtt |= BB.RookAttacks(OccBb(p) ^ p->StraightMovers(sd), sq);
    if (bbAtt & bbZone) {
      wood++;
      att += king_att[Q] * BB.PopCnt(bbAtt & bbZone);
    }

    // Queen on 7th rank

    if (SqBb(sq) & bbRelRank[sd][RANK_7]) {
      if (p->Pawns(op) & bbRelRank[sd][RANK_7]
      || p->Kings(op) & bbRelRank[sd][RANK_8]) {
        Add(sd, F_LINES, queenOnSeventhMg, queenOnSeventhEg);
      }
    }

  } // end of queen eval
  
  // Score terms using information gathered during piece eval

  if (r_on_7th == 2)          // two rooks on 7th rank
    Add(sd, F_LINES, twoRooksOn7thMg, twoRooksOn7thEg); 

  // Score king attacks if own queen is present and there are at least 2 attackers

  if (wood > 1 && p->cnt[sd][Q]) {
    if (att > 399) att = 399;
    tmp = danger[att];
    Add(sd, F_ATT, tmp, tmp);
  }

}

void cEval::ScoreOutpost(int sd, int pc, int sq) {

  int mul = 0;
  int tmp = sp_pst_data[sd][pc][sq];
  if (tmp) {
    if (SqBb(sq) & ~bbPawnCanTake[Opp(sd)]) mul += 2;  // in the hole of enemy pawn structure
    if (SqBb(sq) & bbPawnTakes[sd]) mul += 1;          // defended by own pawn
    if (SqBb(sq) & bbTwoPawnsTake[sd]) mul += 1;       // defended by two pawns
    tmp *= mul;
    tmp /= 2;

    Add(sd, F_OUTPOST, tmp, tmp);
  }
}

void cEval::ScoreHanging(POS *p, int sd) {

  int pc, sq, sc;
  int op = Opp(sd);
  U64 bbHanging = p->cl_bb[op]    & ~bbPawnTakes[op];
  U64 bbThreatened = p->cl_bb[op] & bbPawnTakes[sd];
  bbHanging |= bbThreatened;      // piece attacked by our pawn isn't well defended
  bbHanging &= bbAllAttacks[sd];  // hanging piece has to be attacked
  bbHanging &= ~p->Pawns(op);     // currently we don't evaluate threats against pawns

  U64 bbDefended = p->cl_bb[op] & bbAllAttacks[op];
  bbDefended &= bbMinorAttacks[sd];
  bbDefended &= ~bbPawnTakes[sd]; // no defense against pawn attack
  bbDefended &= ~p->Pawns(op);    // currently we don't evaluate threats against pawns

  // hanging pieces (attacked and undefended)

  while (bbHanging) {
    sq = BB.PopFirstBit(&bbHanging);
    pc = TpOnSq(p, sq);
    sc = tp_value[pc] / 64;
    Add(sd, F_PRESSURE, 10 + sc, 18 + sc);
  }

  // defended pieces under attack

  while (bbDefended) {
    sq = BB.PopFirstBit(&bbDefended);
    pc = TpOnSq(p, sq);
    sc = tp_value[pc] / 96;
    Add(sd, F_PRESSURE, 5 + sc, 9 + sc);
  }
}

void cEval::ScorePassers(POS * p, int sd) 
{
  U64 bbPieces = p->Pawns(sd);
  int sq, mul, mg_tmp, eg_tmp;
  int op = Opp(sd);
  U64 bbOwnPawns = p->Pawns(sd);
  U64 bbStop;

  while (bbPieces) {
    sq = BB.PopFirstBit(&bbPieces);

    // Passed pawn

    if (!(passed_mask[sd][sq] & p->Pawns(op))) {

      bbStop = ShiftFwd(SqBb(sq), sd);
      mg_tmp = passed_bonus_mg[sd][Rank(sq)];
      eg_tmp = passed_bonus_eg[sd][Rank(sq)] - ((passed_bonus_eg[sd][Rank(sq)] * dist[sq][p->king_sq[op]]) / 30);
      mul = 100;

      // blocked passers score less

      if (bbStop & OccBb(p)) mul -= 20; // TODO: only with a blocker of opp color

      // our control of stop square
    
      else if ( (bbStop & bbAllAttacks[sd]) 
      &&   (bbStop & ~bbAllAttacks[op]) ) mul += 10;
   
      Add(sd, F_PASSERS, (mg_tmp * mul) / 100, (eg_tmp * mul) / 100);
    }
  }
}

int ChebyshevDistance(int sq1, int sq2) {

  int file1, file2, rank1, rank2;
  int rankDistance, fileDistance;
  file1 = sq1 & 7;
  file2 = sq2 & 7;
  rank1 = sq1 >> 3;
  rank2 = sq2 >> 3;
  rankDistance = abs(rank2 - rank1);
  fileDistance = abs(file2 - file1);
  return Max(rankDistance, fileDistance);
}

void cEval::ScoreUnstoppable(POS * p) {

  U64 bbPieces, bbSpan, bbProm;
  int w_dist = 8;
  int b_dist = 8;
  int sq, ksq, psq, tempo, prom_dist;

  // Function loses Elo if it is used in endgames with pieces.
  // Is it a speed issue or a problem with logic?

  if (!PcMatNone(p, WC) || !PcMatNone(p, BC)) return;

  // White unstoppable passers

  ksq = KingSq(p, BC);
  if (p->side == BC) tempo = 1; else tempo = 0;
  bbPieces = p->Pawns(WC);
  while (bbPieces) {
    sq = BB.PopFirstBit(&bbPieces);
    if (!(passed_mask[WC][sq] & p->Pawns(BC))) {
      bbSpan = GetFrontSpan(SqBb(sq), WC);
      psq = ((WC - 1) & 56) + (sq & 7);
      prom_dist = Min(5, ChebyshevDistance(sq, psq));

      if ( prom_dist < (ChebyshevDistance(ksq, psq) - tempo)) {
        if (bbSpan & p->Kings(WC)) prom_dist++;
        w_dist = Min(w_dist, prom_dist);
      }
    }
  }

  // Black unstoppable passers

  ksq = KingSq(p, WC);
  if (p->side == WC) tempo = 1; else tempo = 0;
  while (bbPieces) {
    sq = BB.PopFirstBit(&bbPieces);
    if (!(passed_mask[BC][sq] & p->Pawns(WC))) {
      bbSpan = GetFrontSpan(SqBb(sq), BC);
      if (bbSpan & p->Kings(WC)) tempo -= 1;
      psq = ((BC - 1) & 56) + (sq & 7);
      prom_dist = Min(5, ChebyshevDistance(sq, psq));

      if (prom_dist < (ChebyshevDistance(ksq, psq) - tempo)) {
        if (bbSpan & p->Kings(BC)) prom_dist++;
        b_dist = Min(b_dist, prom_dist);
      }
    }
  }

  // current function is too stupid to evaluate pawn races properly,
  // so we add a bonus only if only one side has an unstoppable passer.

  if (w_dist < b_dist && b_dist == 8) Add(WC, F_PASSERS, 0, 500);
  if (b_dist < w_dist && w_dist == 8) Add(BC, F_PASSERS, 0, 500);
}

int cEval::Return(POS *p, int use_hash) {

  assert(prog_side == WC || prog_side == BC);

  // Try to retrieve score from eval hashtable

  int addr = p->hash_key % EVAL_HASH_SIZE;

  if (EvalTT[addr].key == p->hash_key && use_hash) {
    int hashScore = EvalTT[addr].score;
    return p->side == WC ? hashScore : -hashScore;
  }

  // Clear eval

  int score = 0;
  int mg_score = 0;
  int eg_score = 0;

  for (int sd = 0; sd < 2; sd++) {
    for (int fc = 0; fc < N_OF_FACTORS; fc++) {
      mg[sd][fc] = 0;
      eg[sd][fc] = 0;
    }
  }

  // Init eval with incrementally updated stuff

  mg[WC][F_PST] = p->mg_pst[WC];
  mg[BC][F_PST] = p->mg_pst[BC];
  eg[WC][F_PST] = p->eg_pst[WC];
  eg[BC][F_PST] = p->eg_pst[BC];

  // Calculate variables used during evaluation

  bbPawnTakes[WC] = GetWPControl(p->Pawns(WC));
  bbPawnTakes[BC] = GetBPControl(p->Pawns(BC));
  bbTwoPawnsTake[WC] = GetDoubleWPControl(p->Pawns(WC));
  bbTwoPawnsTake[BC] = GetDoubleBPControl(p->Pawns(BC));
  bbAllAttacks[WC] = bbPawnTakes[WC] | BB.KingAttacks(p->king_sq[WC]);
  bbAllAttacks[BC] = bbPawnTakes[BC] | BB.KingAttacks(p->king_sq[BC]);
  bbMinorAttacks[WC] = bbMinorAttacks[BC] = 0ULL;
  bbPawnCanTake[WC] = BB.FillNorth(bbPawnTakes[WC]);
  bbPawnCanTake[BC] = BB.FillSouth(bbPawnTakes[BC]);

  // Tempo bonus

  Add(p->side, F_OTHERS, 10, 5);

  // Evaluate pieces and pawns

  ScorePieces(p, WC);
  ScorePieces(p, BC);
  FullPawnEval(p, use_hash);
  ScoreHanging(p, WC);
  ScoreHanging(p, BC);
  ScorePatterns(p);
  ScorePassers(p, WC);
  ScorePassers(p, BC);
  ScoreUnstoppable(p);

  // Add stylistic asymmetric stuff

  mg[prog_side][F_OTHERS] += keep_queen  * p->cnt[prog_side][Q];
  mg[prog_side][F_OTHERS] += keep_rook   * p->cnt[prog_side][R];
  mg[prog_side][F_OTHERS] += keep_bishop * p->cnt[prog_side][B];
  mg[prog_side][F_OTHERS] += keep_knight * p->cnt[prog_side][N];
  mg[prog_side][F_OTHERS] += keep_pawn   * p->cnt[prog_side][P];

  // Sum all the symmetric eval factors
  // (we start from 2 so that we won't touch king attacks 
  // and mobility, both of which are asymmetric)

  for (int fc = 2; fc < N_OF_FACTORS; fc++) {
    mg_score += (mg[WC][fc] - mg[BC][fc]) * weights[fc] / 100;
    eg_score += (eg[WC][fc] - eg[BC][fc]) * weights[fc] / 100;
  }

  // Add asymmetric eval factors

  mg_score += mg[WC][F_ATT] * curr_weights[WC][SD_ATT] / 100;
  mg_score -= mg[BC][F_ATT] * curr_weights[BC][SD_ATT] / 100;
  eg_score += eg[WC][F_ATT] * curr_weights[WC][SD_ATT] / 100;
  eg_score -= eg[BC][F_ATT] * curr_weights[BC][SD_ATT] / 100;

  mg_score += mg[WC][F_MOB] * curr_weights[WC][SD_MOB] / 100;
  mg_score -= mg[BC][F_MOB] * curr_weights[BC][SD_MOB] / 100;
  eg_score += eg[WC][F_MOB] * curr_weights[WC][SD_MOB] / 100;
  eg_score -= eg[BC][F_MOB] * curr_weights[BC][SD_MOB] / 100;

  // Merge mg/eg scores

  int mg_phase = Min(max_phase, p->phase);
  int eg_phase = max_phase - mg_phase;

  score += (((mg_score * mg_phase) + (eg_score * eg_phase)) / max_phase);

  // Material imbalance table

  int minorBalance = p->cnt[WC][N] - p->cnt[BC][N] + p->cnt[WC][B] - p->cnt[BC][B];
  int majorBalance = p->cnt[WC][R] - p->cnt[BC][R] + 2 * p->cnt[WC][Q] - 2 * p->cnt[BC][Q];

  int x = Max(majorBalance + 4, 0);
  if (x > 8) x = 8;

  int y = Max(minorBalance + 4, 0);
  if (y > 8) y = 8;

  score += SCALE(imbalance[x][y], mat_perc);

  score += CheckmateHelper(p);

  // Scale down drawish endgames

  int draw_factor = 64;
  if (score > 0) draw_factor = GetDrawFactor(p, WC);
  else           draw_factor = GetDrawFactor(p, BC);
  score *= draw_factor;
  score /= 64;

  // Make sure eval doesn't exceed mate score

  if (score < -MAX_EVAL)
    score = -MAX_EVAL;
  else if (score > MAX_EVAL)
    score = MAX_EVAL;

  // Weakening: add pseudo-random value to eval score

  if (eval_blur) {
    int randomMod = (eval_blur / 2) - (p->hash_key % eval_blur);
    score += randomMod;
  }

  // Save eval score in the evaluation hash table

  EvalTT[addr].key = p->hash_key;
  EvalTT[addr].score = score;

  // Return score relative to the side to move

  return p->side == WC ? score : -score;
}

void cEval::Add(int sd, int factor, int mg_bonus, int eg_bonus) {

  mg[sd][factor] += mg_bonus;
  eg[sd][factor] += eg_bonus;
}

void cEval::Print(POS * p) {

  int mg_score, eg_score, total;
  int mg_phase = Min(max_phase, p->phase);
  int eg_phase = max_phase - mg_phase;

  // BUG: eval asymmetry not shown
  // TODO: fix me!

  printf("Total score: %d\n", Return(p, 0));
  printf("-----------------------------------------------------------------\n");
  printf("Factor     | Val (perc) |   Mg (  WC,   BC) |   Eg (  WC,   BC) |\n");
  printf("-----------------------------------------------------------------\n");
  for (int fc = 0; fc < N_OF_FACTORS; fc++) {
  mg_score = ((mg[WC][fc] - mg[BC][fc]) * weights[fc]) / 100;
  eg_score = ((eg[WC][fc] - eg[BC][fc]) * weights[fc]) / 100;
  total = (((mg_score * mg_phase) + (eg_score * eg_phase)) / max_phase);

    printf(factor_name[fc]);
    printf(" | %4d (%3d) | %4d (%4d, %4d) | %4d (%4d, %4d) |\n", total, weights[fc], mg_score, mg[WC][fc], mg[BC][fc], eg_score, eg[WC][fc], eg[BC][fc]);
  }
  printf("-----------------------------------------------------------------\n");
}