﻿/*=======================================================================
*
*   Copyright (C) 2013 Lysine.
*
*   Filename:    Danmaku.cpp
*   Time:        2013/03/18
*   Author:      Lysine
*
*   Lysine is a student majoring in Software Engineering
*   from the School of Software, SUN YAT-SEN UNIVERSITY.
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.

*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
=========================================================================*/

#include "Danmaku.h"
#include "APlayer.h"
#include "Config.h"
#include "Editor.h"
#include "Graphic.h"
#include "Load.h"
#include "Local.h"
#include "Render.h"
#include "Shield.h"
#include <algorithm>

#define qThreadPool QThreadPool::globalInstance()

Danmaku *Danmaku::ins=nullptr;

Danmaku *Danmaku::instance()
{
	return ins?ins:new Danmaku(qApp);
}

Danmaku::Danmaku(QObject *parent):
	QAbstractItemModel(parent)
{
	ins=this;
	setObjectName("Danmaku");
	cur=time=0;
	qThreadPool->setMaxThreadCount(Config::getValue("/Danmaku/Thread",QThread::idealThreadCount()));
	connect(APlayer::instance(),&APlayer::jumped,     this,&Danmaku::jumpToTime);
	connect(APlayer::instance(),&APlayer::timeChanged,this,&Danmaku::setTime   );
	connect(this,SIGNAL(layoutChanged()),Render::instance(),SLOT(draw()));
	QMetaObject::invokeMethod(this,"alphaChanged",Qt::QueuedConnection,Q_ARG(int,Config::getValue("/Danmaku/Alpha",100)));
}

Danmaku::~Danmaku()
{
	qThreadPool->clear();
	qThreadPool->waitForDone();
	qDeleteAll(current);
}

void Danmaku::draw(QPainter *painter,qint64 move)
{
	QVarLengthArray<Graphic *> dirty;
	lock.lockForWrite();
	dirty.reserve(current.size());
	for(auto iter=current.begin();iter!=current.end();){
		Graphic *g=*iter;
		if(g->move(move)){
			dirty.append(g);
			++iter;
		}
		else{
			delete g;
			iter=current.erase(iter);
		}
	}
	lock.unlock();
	for(Graphic *g:dirty){
		g->draw(painter);
	}
}

QVariant Danmaku::data(const QModelIndex &index,int role) const
{
	if(index.isValid()){
		const Comment &comment=*danmaku[index.row()];
		switch(role){
		case Qt::DisplayRole:
			if(index.column()==0){
				if(comment.blocked){
					return tr("Blocked");
				}
				else{
					QString time("%1:%2");
					qint64 sec=comment.time/1000;
					if(sec<0){
						time.prepend("-");
						sec=-sec;
					}
					time=time.arg(sec/60,2,10,QChar('0'));
					time=time.arg(sec%60,2,10,QChar('0'));
					return time;
				}
			}
			else{
				if(comment.mode==7){
					QJsonDocument doc=QJsonDocument::fromJson(comment.string.toUtf8());
					if(doc.isArray()){
						QJsonArray data=doc.array();
						return data.size()>=5?data.at(4).toString():QString();
					}
					else{
						return doc.object()["n"].toString();
					}
				}
				else{
					return comment.string.left(50).remove('\n');
				}
			}
		case Qt::ForegroundRole:
			if(index.column()==0){
				if(comment.blocked||comment.time>=60000000){
					return QColor(Qt::red);
				}
			}
			else{
				if(comment.blocked){
					return QColor(Qt::gray);
				}
			}
			break;
		case Qt::ToolTipRole:
			if(index.column()==1){
				return Qt::convertFromPlainText(comment.string);
			}
			break;
		case Qt::TextAlignmentRole:
			if(index.column()==0){
				return Qt::AlignCenter;
			}
			break;
		case Qt::BackgroundRole:
			switch(comment.mode){
			case 7:
				return QColor(200,255,200);
			case 8:
				return QColor(255,255,160);
			default:
				break;
			}
		case Qt::UserRole:
			return (quintptr)&comment;
		}
	}
	return QVariant();
}

int Danmaku::rowCount(const QModelIndex &parent) const
{
	return parent.isValid()?0:danmaku.size();
}

int Danmaku::columnCount(const QModelIndex &parent) const
{
	return parent.isValid()?0:2;
}

QModelIndex Danmaku::parent(const QModelIndex &) const
{
	return QModelIndex();
}

QModelIndex Danmaku::index(int row,int colum,const QModelIndex &parent) const
{
	if(!parent.isValid()&&colum<2){
		return createIndex(row,colum);
	}
	return QModelIndex();
}

QVariant Danmaku::headerData(int section,Qt::Orientation orientation,int role) const
{
	if(role==Qt::DisplayRole&&orientation==Qt::Horizontal){
		if(section==0){
			return tr("Time");
		}
		if(section==1){
			return tr("Comment");
		}
	}
	return QVariant();
}

const Comment *Danmaku::commentAt(QPoint point) const
{
	lock.lockForRead();
	for(Graphic *g:current){
		if(g->currentRect().contains(point)){
			lock.unlock();
			return g->getSource();
		}
	}
	lock.unlock();
	return nullptr;
}

void Danmaku::setAlpha(int _alpha)
{
	Config::setValue("/Danmaku/Alpha", _alpha);
	emit alphaChanged(_alpha);
}

void Danmaku::resetTime()
{
	cur=0;
	time=0;
}

void Danmaku::clearPool()
{
	if(!pool.isEmpty()){
		clearCurrent();
		pool.clear();
		danmaku.clear();
		parse(0x1|0x2);
	}
}

namespace
{
class CommentPointer
{
public:
	const Comment *comment;

	CommentPointer(const Comment *comment):
		comment(comment)
	{
	}

	inline bool operator == (const CommentPointer &o) const
	{
		return *comment==*o.comment;
	}
};

inline uint qHash(const CommentPointer &p, uint seed = 0)
{
	return ::qHash(*p.comment,seed);
}
}

void Danmaku::appendToPool(const Record *record)
{
	Record *append=0;
	for(Record &r:pool){
		if (r.source==record->source){
			append=&r;
			break;
		}
	}
	if(!append){
		pool.append(*record);
		QSet<CommentPointer> s;
		auto &d=pool.last().danmaku;
		for(auto iter=d.begin();iter!=d.end();){
			CommentPointer p(&(*iter));
			if(!s.contains(p)){
				++iter;
				s.insert(p);
			}
			else{
				iter=d.erase(iter);
			}
		}
	}
	else{
		auto &d=append->danmaku;
		QSet<CommentPointer> s;
		for(const Comment &c:d){
			s.insert(&c);
		}
		for(Comment c:record->danmaku){
			c.time+=append->delay-record->delay;
			if(!s.contains(&c)){
				d.append(c);
				s.insert(&d.last());
			}
		}
		if (record->full){
			append->full=true;
		}
	}
	parse(0x1|0x2);
	if(!append&&Load::instance()->size()<2&&pool.size()>=2){
		Editor::exec(lApp->mainWidget());
	}
}

namespace
{
class Compare
{
public:
	inline bool operator ()(const Comment *c,qint64 time)
	{
		return c->time<time;
	}
	inline bool operator ()(qint64 time,const Comment *c)
	{
		return time<c->time;
	}
	inline bool operator ()(const Comment *f,const Comment *s)
	{
		return f->time<s->time;
	}
};
}

void Danmaku::appendToPool(QString source,const Comment *comment)
{
	Record *append=nullptr;
	for(Record &r:pool){
		if (r.source==source){
			append=&r;
			break;
		}
	}
	if(!append){
		Record r;
		r.source=source;
		pool.append(r);
		append=&pool.last();
	}
	append->danmaku.append(*comment);
	auto ptr=&append->danmaku.last();
	danmaku.insert(std::upper_bound(danmaku.begin(),danmaku.end(),ptr,Compare()),ptr);
	parse(0x2);
}

void Danmaku::clearCurrent(bool soft)
{
	qThreadPool->clear();
	qThreadPool->waitForDone();
	lock.lockForWrite();
	for(auto iter=current.begin();iter!=current.end();){
		Graphic *g=*iter;
		if(soft&&g->stay()){
			++iter;
		}
		else{
			delete g;
			iter=current.erase(iter);
		}
	}
	lock.unlock();
	Render::instance()->draw();
}

void Danmaku::insertToCurrent(Graphic *graphic,int index)
{
	lock.lockForWrite();
	graphic->setIndex();
	int size=current.size(),next;
	if (size==0||index==0){
		next=0;
	}
	else{
		int ring=size+1;
		next=index>0?(index%ring):(ring+index%ring);
		if (next==0){
			next=size;
		}
	}
	current.insert(next,graphic);
	lock.unlock();
}

void Danmaku::parse(int flag)
{
	if((flag&0x1)>0){
		beginResetModel();
		danmaku.clear();
		for(Record &record:pool){
			for(Comment &comment:record.danmaku){
				danmaku.append(&comment);
			}
		}
		std::stable_sort(danmaku.begin(),danmaku.end(),Compare());
		jumpToTime(time);
		endResetModel();
	}
	if((flag&0x2)>0){
		for(Record &r:pool){
			for(Comment &c:r.danmaku){
				c.blocked=r.limit!=0&&c.date>r.limit;
			}
		}
		QSet<QString> set;
		int l=Config::getValue("/Shield/Limit",5);
		QVector<QString> clean;
		clean.reserve(danmaku.size());
		if(l!=0){
			for(const Comment *c:danmaku){
				QString r;
				r.reserve(c->string.length());
				for(const QChar &i:c->string){
					if(i.isLetterOrNumber()||i.isMark()||i=='_'){
						r.append(i);
					}
				}
				clean.append(r);
			}
			QHash<QString,int> count;
			int sta=0,end=sta;
			while(end!=danmaku.size()){
				while(danmaku[sta]->time+10000<danmaku[end]->time){
					if(--count[clean[sta]]==0){
						count.remove(clean[sta]);
					}
					++sta;
				}
				if(++count[clean[end]]>l&&danmaku[end]->mode<=6){
					set.insert(clean[end]);
				}
				++end;
			}
		}
		for(int i=0;i<danmaku.size();++i){
			Comment &c=*danmaku[i];
			c.blocked=c.blocked||(l==0?false:set.contains(clean[i]))||Shield::isBlocked(c);
		}
		qThreadPool->clear();
		qThreadPool->waitForDone();
		lock.lockForWrite();
		for(auto iter=current.begin();iter!=current.end();){
			const Comment *cur=(*iter)->getSource();
			if(cur&&cur->blocked){
				delete *iter;
				iter=current.erase(iter);
			}
			else{
				++iter;
			}
		}
		lock.unlock();
		emit layoutChanged();
	}
}

namespace
{
class Process:public QRunnable
{
public:
	Process(QReadWriteLock *l,QList<Graphic *> &c,const QList<const Comment *> &w):
		current(c),lock(l),wait(w)
	{
		createTime=QDateTime::currentMSecsSinceEpoch();
	}

	void run()
	{
		//跳过500毫秒以上未处理的弹幕
		if (wait.isEmpty()||createTime<QDateTime::currentMSecsSinceEpoch()-500){
			return;
		}
		//子线程默认优先级和主线程相同，会导致卡顿
		QThread::currentThread()->setPriority(QThread::NormalPriority);
		QList<Graphic *> ready;
		while(!wait.isEmpty()){
			const Comment *comment=wait.takeFirst();
			Graphic *graphic=Graphic::create(*comment);
			if(!graphic){
				//自带弹幕系统未识别，通知插件处理
				emit Danmaku::instance()->unrecognizedComment(comment);
				continue;
			}
			QRectF &rect=graphic->currentRect();
			const auto &locate=graphic->locate();
			switch(locate.size()){
			case 1:
				//图元指定位置
				rect=locate.first();
			case 0:
				//弹幕自行定位
				ready.append(graphic);
				lock->lockForWrite();
				break;
			default:
			{
				//弹幕自动定位
				QVarLengthArray<int> result(locate.size());
				memset(result.data(),0,sizeof(int)*result.size());
				//弹幕分组高度
				const int slot=40;
				//计算每个位置的拥挤程度
				auto calculate=[&](const QList<Graphic *> &data){
					//将弹幕按高度分组，提高查询效率
					QMap<int,QList<Graphic *>> parse;
					for(Graphic *iter:data){
						const QRectF &rect=iter->currentRect();
						int m=rect.top()/slot,n=(rect.bottom()+slot-1)/slot;
						for(;m<n;++m){
							if (iter->getMode()==comment->mode){
								parse[m].append(iter);
							}
						}
					}
					int i=0;
					for(const QRectF &iter:locate){
						rect=iter;
						//查找附近可能重叠的弹幕组
						int m=rect.top()/slot,n=(rect.bottom()+slot-1)/slot;
						QList<Graphic *> close;
						for(auto it=parse.lowerBound(m);it!=parse.end()&&it.key()<=n;++it){
							close.append(*it);
						}
						//弹幕可能跨多个组，去除重复
						std::sort(close.begin(),close.end());
						auto tail=std::unique(close.begin(),close.end());
						//计算交叉面积
						for(auto iter=close.begin();iter!=tail;++iter){
							result[i]+=graphic->intersects(*iter);
						}
						++i;
					}
				};
				//获取读锁，计算现有弹幕的拥挤程度
				lock->lockForRead();
				quint64 last=current.isEmpty()?0:current.last()->getIndex();
				calculate(current);
				lock->unlock();
				ready.append(graphic);
				//获取写锁，计算两次锁之间的新弹幕
				lock->lockForWrite();
				QList<Graphic *> addtion;
				QListIterator<Graphic *> iter(current);
				iter.toBack();
				while(iter.hasPrevious()){
					Graphic *p=iter.previous();
					if(p->getIndex()>last){
						addtion.prepend(p);
					}
					else break;
				}
				calculate(addtion);
				//挑选最空闲的位置
				int thin;
				thin=result[0];
				rect=locate[0];
				for(int i=1;thin!=0&&i<result.size();++i){
					if (thin>result[i]){
						thin=result[i];
						rect=locate[i];
					}
				}
			}
			}
			//相同内容的弹幕需要同时启动，先将其冻结
			graphic->setEnabled(false);
			graphic->setIndex();
			current.append(graphic);
			lock->unlock();
		}
		lock->lockForWrite();
		for(Graphic *iter:ready){
			iter->setEnabled(true);
		}
		lock->unlock();
	}

	Process &operator=(const Process &)=delete;

private:
	QList<Graphic *> &current;
	qint64 createTime;
	QReadWriteLock *lock;
	QList<const Comment *> wait;
};
}

void Danmaku::setTime(qint64 _time)
{
	time=_time;
	int l=Config::getValue("/Shield/Density",0),n=0;
	QMap<qint64,QMap<QString,QList<const Comment *>>> buffer;
	for(;cur<danmaku.size()&&danmaku[cur]->time<time;++cur){
		const Comment *c=danmaku[cur];
		if(!c->blocked&&(c->mode>6||l==0||current.size()+n<l)){
			++n;
			buffer[c->time][c->string].append(c);
		}
	}
	for(const auto &sameTime:buffer){
		for(const auto &sameText:sameTime){
			qThreadPool->start(new Process(&lock,current,sameText));
		}
	}
}

void Danmaku::delayAll(qint64 _time)
{
	for(Record &r:pool){
		r.delay+=_time;
		for(Comment &c:r.danmaku){
			c.time+=_time;
		}
	}
	jumpToTime(time);
	emit layoutChanged();
}

void Danmaku::jumpToTime(qint64 _time)
{
	clearCurrent(true);
	time=_time;
	cur=std::lower_bound(danmaku.begin(),danmaku.end(),time,Compare())-danmaku.begin();
}

void Danmaku::saveToFile(QString file)
{
	QFile f(file);
	f.open(QIODevice::WriteOnly|QIODevice::Text);
	bool skip=Config::getValue("/Interface/Save/Skip",false);
	if (file.endsWith("xml",Qt::CaseInsensitive)){
		QXmlStreamWriter w(&f);
		w.setAutoFormatting(true);
		w.writeStartDocument();
		w.writeStartElement("i");
		w.writeStartElement("chatserver");
		w.writeCharacters("chat."+Utils::customUrl(Utils::Bilibili));
		w.writeEndElement();
		w.writeStartElement("mission");
		w.writeCharacters("0");
		w.writeEndElement();
		w.writeStartElement("source");
		w.writeCharacters("k-v");
		w.writeEndElement();
		for(const Comment *c:danmaku){
			if(c->blocked&&skip){
				continue;
			}
			w.writeStartElement("d");
			QStringList l;
			l<<QString::number(c->time/1000.0)<<
			   QString::number(c->mode)<<
			   QString::number(c->font)<<
			   QString::number(c->color)<<
			   QString::number(c->date)<<
			   "0"<<
			   c->sender<<
			   "0";
			w.writeAttribute("p",l.join(','));
			w.writeCharacters(c->string);
			w.writeEndElement();
		}
		w.writeEndElement();
		w.writeEndDocument();
	}
	else{
		QJsonArray a;
		for(const Comment *c:danmaku){
			if(c->blocked&&skip){
				continue;
			}
			QJsonObject o;
			QStringList l;
			l<<QString::number(c->time/1000.0)<<
			   QString::number(c->color)<<
			   QString::number(c->mode)<<
			   QString::number(c->font)<<
			   c->sender<<
			   QString::number(c->date);
			o["c"]=l.join(',');
			o["m"]=c->string;
			a.append(o);
		}
		f.write(QJsonDocument(a).toJson(QJsonDocument::Compact));
	}
	f.close();
}
