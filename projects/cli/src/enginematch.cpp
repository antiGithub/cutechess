/*
    This file is part of Cute Chess.

    Cute Chess is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Cute Chess is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Cute Chess.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "enginematch.h"
#include <cmath>
#include <QMultiMap>
#include <chessplayer.h>
#include <playerbuilder.h>
#include <chessgame.h>
#include <polyglotbook.h>
#include <tournament.h>
#include <gamemanager.h>
#include <sprt.h>


EngineMatch::EngineMatch(Tournament* tournament, QObject* parent)
	: QObject(parent),
	  m_tournament(tournament),
	  m_debug(false),
	  m_ratingInterval(0)
{
	Q_ASSERT(tournament != 0);

	m_startTime.start();
}

EngineMatch::~EngineMatch()
{
	qDeleteAll(m_books);
}

OpeningBook* EngineMatch::addOpeningBook(const QString& fileName)
{
	if (fileName.isEmpty())
		return 0;

	if (m_books.contains(fileName))
		return m_books[fileName];

	PolyglotBook* book = new PolyglotBook;
	if (!book->read(fileName))
	{
		delete book;
		qWarning("Can't read opening book file %s", qPrintable(fileName));
		return 0;
	}

	m_books[fileName] = book;
	return book;
}

void EngineMatch::start()
{
	connect(m_tournament, SIGNAL(finished()),
		this, SLOT(onTournamentFinished()));
	connect(m_tournament, SIGNAL(gameStarted(ChessGame*, int, int, int)),
		this, SLOT(onGameStarted(ChessGame*, int)));
	connect(m_tournament, SIGNAL(gameFinished(ChessGame*, int, int, int)),
		this, SLOT(onGameFinished(ChessGame*, int)));

	if (m_debug)
		connect(m_tournament->gameManager(), SIGNAL(debugMessage(QString)),
			this, SLOT(print(QString)));

	QMetaObject::invokeMethod(m_tournament, "start", Qt::QueuedConnection);
}

void EngineMatch::stop()
{
	QMetaObject::invokeMethod(m_tournament, "stop", Qt::QueuedConnection);
}

void EngineMatch::setDebugMode(bool debug)
{
	m_debug = debug;
}

void EngineMatch::setRatingInterval(int interval)
{
	Q_ASSERT(interval >= 0);
	m_ratingInterval = interval;
}

void EngineMatch::onGameStarted(ChessGame* game, int number)
{
	Q_ASSERT(game != 0);

	qDebug("Started game %d of %d (%s vs %s)",
	       number,
	       m_tournament->finalGameCount(),
	       qPrintable(game->player(Chess::Side::White)->name()),
	       qPrintable(game->player(Chess::Side::Black)->name()));
}

void EngineMatch::onGameFinished(ChessGame* game, int number)
{
	Q_ASSERT(game != 0);

	Chess::Result result(game->result());
	qDebug("Finished game %d (%s vs %s): %s",
	       number,
	       qPrintable(game->player(Chess::Side::White)->name()),
	       qPrintable(game->player(Chess::Side::Black)->name()),
	       qPrintable(result.toVerboseString()));

	if (m_tournament->playerCount() == 2)
	{
		Tournament::PlayerData fcp = m_tournament->playerAt(0);
		Tournament::PlayerData scp = m_tournament->playerAt(1);
		int totalResults = fcp.wins + fcp.losses + fcp.draws;
		qDebug("Score of %s vs %s: %d - %d - %d  [%.3f] %d",
		       qPrintable(fcp.builder->name()),
		       qPrintable(scp.builder->name()),
		       fcp.wins, scp.wins, fcp.draws,
		       double(fcp.wins * 2 + fcp.draws) / (totalResults * 2),
		       totalResults);
	}

	if (m_ratingInterval != 0
	&&  (m_tournament->finishedGameCount() % m_ratingInterval) == 0)
		printRanking();
}

void EngineMatch::onTournamentFinished()
{
	if (m_ratingInterval == 0
	||  m_tournament->finishedGameCount() % m_ratingInterval != 0)
		printRanking();

	QString error = m_tournament->errorString();
	if (!error.isEmpty())
		qWarning("%s", qPrintable(error));

	qDebug("Finished match");
	connect(m_tournament->gameManager(), SIGNAL(finished()),
		this, SIGNAL(finished()));
	m_tournament->gameManager()->finish();
}

void EngineMatch::print(const QString& msg)
{
	qDebug("%lld %s", m_startTime.elapsed(), qPrintable(msg));
}

struct RankingData
{
	QString name;
	int games;
	qreal score;
	qreal draws;
};

void EngineMatch::printRanking()
{
	QMultiMap<qreal, RankingData> ranking;

	for (int i = 0; i < m_tournament->playerCount(); i++)
	{
		Tournament::PlayerData player(m_tournament->playerAt(i));

		int score = player.wins * 2 + player.draws;
		int total = (player.wins + player.losses + player.draws) * 2;
		if (total <= 0)
			continue;

		qreal ratio = qreal(score) / qreal(total);
		qreal eloDiff = -400.0 * std::log(1.0 / ratio - 1.0) / std::log(10.0);

		if (m_tournament->playerCount() == 2)
		{
			qDebug("ELO difference: %.0f", eloDiff);
			break;
		}

		RankingData data = { player.builder->name(),
				     total / 2,
				     ratio,
				     qreal(player.draws * 2) / qreal(total) };
		ranking.insert(-eloDiff, data);
	}

	if (!ranking.isEmpty())
		qDebug("%4s %-25.25s %7s %7s %7s %7s",
		       "Rank", "Name", "ELO", "Games", "Score", "Draws");

	int rank = 0;
	QMultiMap<qreal, RankingData>::const_iterator it;
	for (it = ranking.constBegin(); it != ranking.constEnd(); ++it)
	{
		const RankingData& data = it.value();
		qDebug("%4d %-25.25s %7.0f %7d %6.0f%% %6.0f%%",
		       ++rank,
		       qPrintable(data.name),
		       -it.key(),
		       data.games,
		       data.score * 100.0,
		       data.draws * 100.0);
	}

	Sprt::Status sprtStatus = m_tournament->sprt()->status();
	if (sprtStatus.llr != 0.0
	||  sprtStatus.lBound != 0.0
	||  sprtStatus.uBound != 0.0)
	{
		QString sprtStr = QString("SPRT: llr %1, lbound %2, ubound %3")
			.arg(sprtStatus.llr, 0, 'g', 3)
			.arg(sprtStatus.lBound, 0, 'g', 3)
			.arg(sprtStatus.uBound, 0, 'g', 3);
		if (sprtStatus.result == Sprt::AcceptH0)
			sprtStr.append(" - H0 was accepted");
		else if (sprtStatus.result == Sprt::AcceptH1)
			sprtStr.append(" - H1 was accepted");

		qDebug("%s", qPrintable(sprtStr));
	}
}
