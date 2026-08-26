#pragma once

#include <chrono>
#include <string>

#include "analysis/basic.h"
#include "analysis/bruteforce/endgame_bruteforce.h"
#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/exact.h"
#include "analysis/rational.h"
#include "analysis/search/midgame_search.h"
#include "analysis/structure.h"
#include "core/config.h"
#include "core/types.h"
#include "ui/game_control.h"

namespace mss {

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
// interactive.h 鈥?鍒嗘瀽/娓告垙鏁版嵁 鈫?UI 鍙秷璐硅〃绀虹殑缈昏瘧灞傦紙鍨冨溇妗讹級銆?
//
// 鍚勭"璁＄畻灏忓瀮鍦?閮芥墧杩欓噷锛氬紩鎿庢棤鍏崇殑姒傜巼鏌ヨ銆佹暣鐩樼墿鍖栥€佺簿纭姣斻€?
// 鍏ㄥ眬鍒嗘瀽锛堝悎娉曟€?+ 鍊欓€夋暟 + 鏆村姏鏋氫妇锛夌瓑銆倁i_app 鍙礋璐ｈ矾鐢?JSON锛?
// GameController 鍙礋璐ｆ父鎴忚鍒欙紙缈诲紑/娉涙椽/鏍囨棗/鑳滆礋锛夈€傛湰灞備笉鍚父鎴?
// 瑙勫垯鎴?HTTP 閫昏緫锛屽叏鏄函璁＄畻銆?
// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€

namespace Interactive {

// 鈹€鈹€ 寮曟搸鏃犲叧鏌ヨ锛堝熀浜庡綋鍓?Analysis 绠＄嚎锛夆攢鈹€

// 鍗曟牸闆锋鐜囷紙Mine鈫?锛孶nknown鈫抰Prob/rho锛孲afe鈫?锛屽墠娌挎牸鈫抌oxProbs锛夈€?
// 闈?const锛歊hoRational 鎯版€ц蹇嗗寲浼氬氨鍦拌ˉ缂撳瓨锛堝箓绛夛紝涓嶅奖鍝嶇粨鏋滐級銆?
inline long double mineProbability(GameController::Analysis& an, int x, int y);

// 鍊欓€夋柟妗堟暟銆?
inline long double candidates(const GameController::Analysis& an);

// 闈炲墠娌匡紙Unknown锛夋牸闆峰瘑搴︺€?
inline long double tCellProbability(const GameController::Analysis& an);

// 鏁寸洏姒傜巼缃戞牸鐗╁寲锛?-based锛屼笌妫嬬洏涓€鑷达級銆傞€愭牸鏌ヨ锛孫(rows*cols)銆?
inline Grid<long double> materializeProbability(GameController::Analysis& an);

// 鐐瑰紑鏌愭牸鐨勭粨鏋滃垎甯冿紙explosion + digit[0..8]锛夛紝璇︽儏闈㈡澘鏌ヨ銆?
inline Probability::ObserveResult observe(GameController::Analysis& an, int x, int y);

// 鈹€鈹€ 涓洏鎼滅储浼氳瘽宸ュ叿 鈹€鈹€

// 浠庝細璇濈墿鍖栨鐜囩綉鏍硷紙1-based锛屼笌妫嬬洏涓€鑷达級銆?
inline Grid<long double> materializeProbability(const MidgameSearch::Session& s);

// 鈹€鈹€ 瀹炵幇鍖?鈹€鈹€

inline long double mineProbability(GameController::Analysis& an, int x, int y) {
    return an.probability().mineProbability(an.state().id(x, y), an.state(),
                                            an.basicMarks(), an.structure());
}

inline long double candidates(const GameController::Analysis& an) {
    return an.probability().candidates;
}

inline long double tCellProbability(const GameController::Analysis& an) {
    return an.probability().tCellProbability;
}

inline Grid<long double> materializeProbability(GameController::Analysis& an) {
    Grid<long double> grid(an.state().rows, an.state().cols, 0.0L);
    for (int x = 1; x <= an.state().rows; ++x)
        for (int y = 1; y <= an.state().cols; ++y)
            grid[x][y] = mineProbability(an, x, y);
    return grid;
}

inline Probability::ObserveResult observe(GameController::Analysis& an, int x, int y) {
    const ObservedBoard& state = an.state();
    const auto& basic = an.basicMarks();
    const auto& structure = an.structure();
    return Exact::observe(state, basic, structure, an.probability(), an.dists(),
                          state.id(x, y));
}

inline Grid<long double> materializeProbability(const MidgameSearch::Session& s) {
    Grid<long double> grid(s.board.rows, s.board.cols, 0.0L);
    for (int x = 1; x <= s.board.rows; ++x)
        for (int y = 1; y <= s.board.cols; ++y)
            grid[x][y] =
                s.prob.mineProbability(s.board.id(x, y), s.board, s.basic, s.structure);
    return grid;
}

}  // namespace Interactive

}  // namespace mss
