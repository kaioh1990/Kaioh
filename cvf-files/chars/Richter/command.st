[Data]
life=39999
power=9999
attack=126
defence=100000
fall.defence_up=50
liedown.time=60
airjuggle=15
sparkno=0000
guard.sparkno=-1
ko.echo=0
volume=0
volumescale=100
intpersistindex=0
floatpersistindex=0
[Size]
xscale=1
yscale=1
ground.back=0
ground.front=0
air.back=0
air.front=0
height=42
attack.dist=7
proj.attack.dist=90
proj.doscale=1
head.pos=-3,-40
mid.pos=-1,-20
shadowoffset=0
draw.offset=0,0
[velocity]
walk.fwd=1.95
walk.back=-1.95
run.fwd=3.2,0
run.back=-5.6,0
jump.neu=0,-7
jump.fwd=2.5,-7
jump.back=2.5,-7
runjump.neu=0;lowest allowed floor level
runjump.fwd=60;walk/fly distance, near
runjump.back=80;walk/fly distance, far
airjump.neu=4,4;max flying speed
airjump.fwd=1;cannot turn during flight, needs input (b+jump)
airjump.back=1;boss - facing while flying/player - doublejumps
air.gethit.groundrecover=-.6,-14
air.gethit.airrecover.mul=.5,.2
air.gethit.airrecover.add=0,-18
air.gethit.airrecover.back=-4
air.gethit.airrecover.fwd=0
air.gethit.airrecover.up=-8
air.gethit.airrecover.down=6
[Movement]
airjump.num=0
airjump.height=35
yaccel=.33
stand.friction=.85
crouch.friction=.82
stand.friction.threshold=8
crouch.friction.threshold=.2
air.gethit.groundlevel=100
air.gethit.groundrecover.ground.threshold=-20
air.gethit.groundrecover.groundlevel=10
air.gethit.airrecover.threshold=-1
air.gethit.airrecover.yaccel=1.4
air.gethit.trip.groundlevel=10
down.bounce.offset=0,20
down.bounce.yaccel=1.6
down.bounce.groundlevel=48
down.friction.threshold=.2
[Remap]
x=a
y=b
z=c
a=x
b=y
c=z
s=s
[Defaults]
command.time=15
command.buffer.time=1
[Command]
name="a"
command=a
time=1
[Command]
name="b"
command=b	
time=1
[Command]
name="c"
command=c
time=1
[Command]
name="x"
command=x
time=1
[Command]
name="y"
command=y
time=1
[Command]
name="z"
command=z
time=1
[Command]
name="start"
command=s
time=1
[Command]
name="holdfwd"
command=/$F
time=1
[Command]
name="holdback"
command=/$B
time=1
[Command]
name="holdup"
command=/$U
time=1
[Command]
name="holddown"
command=/$D
time=1
[Command]
name="holda"
command=/a
time=1
[Command]
name="holdb"
command=/b
time=1
[Command]
name="holdc"
command=/c
time=1
[Command]
name="holdx"
command=/x
time=1
[Command]
name="holdy"
command=/y
time=1
[Command]
name="holdz"
command=/z
time=1
[Command]
name="holdstart"
command=/s
time=1
[Command]
name="recovery"
command=x+y
time=1
[Command]
name="a2"
command=a
time=1
[Command]
name="b2"
command=b
time=1
[Command]
name="c2"
command=c
time=1
[Command]
name="x2"
command=x
time=1
[Command]
name="y2"
command=y
time=1
[Command]
name="z2"
command=z
time=1
[Command]
name="start2"
command=s
time=1
[Command]
name="holdfwd2"
command=/$F
time=1
[Command]
name="holdback2"
command=/$B
time=1
[Command]
name="holdup2"
command=/$U
time=1
[Command]
name="holddown2"
command=/$D
time=1
[Command]
name="holda2"
command=/a
time=1
[Command]
name="holdb2"
command=/b
time=1
[Command]
name="holdc2"
command=/c
time=1
[Command]
name="holdx2"
command=/x
time=1
[Command]
name="holdy2"
command=/y
time=1
[Command]
name="holdz2"
command=/z
time=1
[Command]
name="holdstart2"
command=/s
time=1
[Command]
name="recovery2"
command=x+y
time=1
[Command]
name="itemcrash"
command=D,F,U,x;>
time=38
[Command]
name="itemcrash"
command=D,F,/$U,x;>
time=38
[Command]
name="itemcrash"
command=D,F,U,z;>
time=38
[Command]
name="itemcrash"
command=D,F,/$U,z;>
time=38
[Command]
name="itemcrash_x"
command=D,F,U,x;>
time=38
[Command]
name="itemcrash_x"
command=D,F,/$U,x;>
time=38
[Command]
name="itemcrash_z"
command=D,F,U,z;>
time=38
[Command]
name="itemcrash_z"
command=D,F,/$U,z;>
time=38
[Command]
name="HCF+a"
command=B,BD,D,DF,F,x;>
time=28
[Command]
name="HCF+b"
command=B,BD,D,DF,F,z;>
time=28
[Command]
name="HCF+a"
command=B,D,DF,x;>
time=28
[Command]
name="HCF+b"
command=B,D,DF,z;>
time=28
[Command]
name="HCF+a"
command=U,UF,F,x;>
time=28
[Command]
name="HCF+b"
command=U,UF,F,z;>
time=28
[Command]
name="UDF+a"
command=U,D,DF,x;>
time=25
[Command]
name="UDF+b"
command=U,D,DF,z;>
time=25
[Command]
name="UDF+a"
command=U,D,DB,x;>
time=25
[Command]
name="UDF+b"
command=U,D,DB,z;>
time=25
[Command]
name="UDF+a"
command=U,B,D,DF,x;>
time=25
[Command]
name="UDF+b"
command=U,B,D,DF,z;>
time=25
[Command]
name="UDF+a"
command=U,D,DF,F,x;>
time=25
[Command]
name="UDF+b"
command=U,D,DF,F,z;>
time=25
[Command]
name="UDF+a"
command=U,D,DB,B,x;>
time=25
[Command]
name="UDF+b"
command=U,D,DB,B,z;>
time=25
[Command]
name="DF+a"
command=D,DF,F,x;>
time=15
[Command]
name="DF+b"
command=D,DF,F,z;>
time=15
[Command]
name="DF+a"
command=D,DB,B,x;>
time=15
[Command]
name="DF+b"
command=D,DB,B,z;>
time=15
[Command]
name="DF+a"
command=D,DF,x;>
time=15
[Command]
name="DF+b"
command=D,DF,z;>
time=15
[Command]
name="DF+a"
command=D,DB,x;>
time=15
[Command]
name="DF+b"
command=D,DB,z;>
time=15
[Command]
name="FF+a"
command=~F,F,x;>
time=14
[Command]
name="FF+b"
command=~F,F,z;>
time=14
[Command]
name="FF+a"
command=~DF,DF,x;>
time=14
[Command]
name="FF+b"
command=~DF,DF,z;>
time=14
[Command]
name="flymode"
command=b
time=1
[Command]
name="FF"
command=F,F
time=16
[Command]
name="BB"
command=B,B
time=16
[Command]
name="BF"
command=B,F
time=16
[Command]
name="back"
command=B
time=1
[Command]
name="fwd"
command=F
time=1
[Command]
name="up"
command=U
time=1
[Command]
name="down"
command=D
time=1
[Command]
name="bc1"
command=$B
time=1
[Command]
name="fw1"
command=$F
time=1
[Command]
name="dw1"
command=$D
time=1
[Command]
name="up1"
command=$U
time=1
[Command]
name="bc2"
command=~B
time=1
[Command]
name="fw2"
command=~F
time=1
[Command]
name="dw2"
command=~D
time=1
[Command]
name="up2"
command=~U
time=1
[Command]
name="jump"
command=y
time=1
[Command]
name="holdjump"
command=/y
time=1
[Command]
name="releasejump"
command=~y
time=1
[Command]
name="attack"
command=x
time=1
[Command]
name="holdattack"
command=/x
time=1
[Command]
name="attack2"
command=z
time=1
[Command]
name="holdattack2"
command=/z
time=1
[Command]
name="L"
command=a
time=1
[Command]
name="holdL"
command=/a
time=1
[Command]
name="R"
command=b
time=1
[Command]
name="holdR"
command=/b
time=1
[Command]
name="D"
command=c
time=1
[Command]
name="holdD"
command=/c
time=1
; online port: Whip Rush on Forward + W (LT on a controller)
[Command]
name="UDF+a"
command=/$F,w
time=1

[Statedef -1]
[State *]
type=changestate
trigger1=ishelper(9741)
value=9741
[State *]
type=changestate
trigger1=ishelper(9742)
value=9742
[State *]
type=changestate
triggerall=numenemy
triggerall=numhelper(54000)&&numhelper(54001)&&enemy,numhelper(54000)&&enemy,numhelper(54001)
triggerall=fvar(09)=0&&roundstate=2&&life>0
triggerall=stateno=0
trigger1=!numpartner
trigger1=(stateno!=[190,199])&&(stateno!=[49000,49999])
trigger2=numpartner
trigger2=id<partner,id
trigger2=(stateno!=[190,199])&&(stateno!=[49000,49999])
trigger3=numpartner
trigger3=id>partner,id
trigger3=(partner,stateno=[190,199])||(partner,stateno=[49000,49999])||(partner,authorname="bonus"&&partner,stateno=5150)||(partner,authorname="bonus"&&((enemy,stateno=[190,199])||(enemy,stateno=[49000,49999])))
trigger3=(stateno!=[190,199])&&(stateno!=[49000,49999])
value=190
ctrl=0
;AI---------------
[State *];cross intro
type=null;changestate
triggerall=numenemy
triggerall=var(57)=0
;triggerall=sysvar(2)=0
triggerall=fvar(36)=[0,10]
triggerall=var(58)>0
triggerall=roundstate=2&&numenemy>0
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1
triggerall=stateno!=1
triggerall=stateno!=[999,1099]
triggerall=(prevstateno=[190,199])||(prevstateno=[49000,49799])||prevstateno=49999||prevstateno=5900
trigger1=stateno=0
value=1009
ctrl=0
[State *]
type=changestate
triggerall=numenemy
triggerall=sysvar(2)=0
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=stateno!=1&&stateno!=1009
triggerall=roundstate=2
triggerall=abs(fvar(36))=0
triggerall=p2dist x>=0
triggerall=!(enemynear,var(26)!=0&&(enemynear,stateno=[1000,1099]))&&enemynear,var(26)=0
triggerall=stateno!=41&&var(58)>0&&abs(p2dist x)<=64&&(p2dist y=[-30,10])&&random>515&&life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=!(enemy,name="Julius"&&(enemy,stateno=1100||(enemy,stateno=[214,219])))&&(stateno!=[53,59])
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)
trigger2=stateno=105&&animelemtime(1)>=8
trigger3=stateno=200&&animelemtime(6)>=0&&movecontact
trigger4=stateno=600&&animelemtime(6)>=0&&statetype=S&&movecontact
value=200+((((enemy,stateno=[10,12])||(enemy,stateno=[400,599])||(stateno=[200,399])||enemy,stateno=4991||enemy,statetype=C||(enemy,selfanimexist(9000130+enemy,var(51))=1))&&var(23)=0&&statetype!=A)*200)+((statetype=A)*400)
ctrl=0
[State *]
type=null
triggerall=numenemy
triggerall=sysvar(2)=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1
triggerall=var(58)>0&&movetype!=H
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=roundstate=2
triggerall=stateno!=[250,260]
triggerall=stateno!=[450,460]
triggerall=stateno!=[650,660]
triggerall=stateno!=[1000,1099]
triggerall=!(enemynear,var(26)!=0&&(enemynear,stateno=[1000,1099]))
trigger1=abs(p2dist x)>=100&&(p2dist y=[-50,20])
trigger1=var(04):=01
trigger2=(p2dist y>=20)||(abs(p2dist y)<50&&p2dist x<70&&enemy,statetype=C)
trigger2=var(04):=02
trigger3=abs(p2dist x)<=100&&(p2dist y=[-50,20])&&enemy,statetype!=C
trigger3=var(04):=03
trigger4=abs(p2dist x)<80&&(p2dist y=[-110,-58])
trigger4=var(04):=04
trigger5=abs(p2dist x)>=80&&(p2dist y=[-90,-48])
trigger5=var(04):=05
[State *]
type=changestate
triggerall=numenemy
triggerall=sysvar(2)=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1
triggerall=var(58)>0&&movetype!=H
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=roundstate=2
triggerall=anim%10000!=49
triggerall=stateno!=44&&stateno!=41
triggerall=(stateno!=[250,259])&&(stateno!=[450,459])&&(stateno!=[650,659])&&(stateno!=[900,999])
triggerall=stateno!=1&&stateno!=1009&&(stateno!=[1000,1099])
triggerall=(p2dist x=[1,140])
triggerall=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||((stateno=[200,209])&&movecontact)||((stateno=[400,409])&&movecontact)||((stateno=[600,609])&&movecontact)||(stateno=48&&animelem=5,>=0)
;triggerall=(ctrl||(stateno=[0,40])||(stateno=[100,106]))
triggerall=abs(fvar(36))=0
trigger1=random>=850
trigger1=var(04)=01&&var(34)>=25&&numhelper(220)<=floor(fvar(32));dagger
trigger1=abs(p2dist x)>96&&(p2dist y=[-50,20])
trigger2=random>=550
trigger2=var(04)=02&&var(34)>=50&&numhelper(230)<=0;water
trigger2=(p2dist y>=20&&p2dist x>=70&&enemy,statetype!=C)||(abs(p2dist y)<50&&p2dist x<70&&enemy,statetype=C)
trigger3=random>=900
trigger3=var(04)=03&&var(34)>=100&&numhelper(240)<=floor(fvar(32));cross
trigger3=abs(p2dist x)<=100&&(p2dist y=[-50,20])&&enemy,statetype!=C
trigger4=random>=850
trigger4=var(04)=04&&var(34)>=30&&numhelper(250)<=floor(fvar(32));axe
trigger4=p2dist y=[-110,-58]
trigger5=random>=850
trigger5=var(04)=05&&var(34)>=35&&numhelper(260)<=floor(fvar(32));book
trigger5=(abs(p2dist x)<80&&(p2dist y=[-48,20]))||(enemynear,stateno=[30,32])||(enemynear,stateno=[900,999])
value=250+(statetype=A*400)
ctrl=0
[State *];reb. stone
type=changestate
triggerall=numenemy
triggerall=var(01)>=100
triggerall=sysvar(2)=0
triggerall=abs(fvar(36))=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0&&var(07)=1&&(pos y>=0||var(27)>0)&&(stateno!=[1350,1355])
triggerall=stateno!=1&&stateno!=[1000,1010]
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=roundstate=2&&numenemy>0&&stateno!=41&&var(58)>0
triggerall=(ctrl||(stateno=[0,40])||(stateno=[100,106]))&&statetype!=A
triggerall=stateno!=[1000,1099]
triggerall=!(enemynear,var(26)!=0&&(enemynear,stateno=[1000,1099]))
trigger1=ifelse(enemy,fvar(38)<=1,(random=[900,950]),ifelse(enemy,fvar(38)=2,(random=[800,900]),ifelse(enemy,fvar(38)=3,(random=[700,850]),random=[680,840])))
trigger1=var(34)>=500
trigger1=p2dist y<=-60
trigger1=abs(p2dist x)<=80
value=1008
ctrl=0
[State *];daggers
type=changestate
triggerall=numenemy
triggerall=var(01)>=100
triggerall=sysvar(2)=0
triggerall=abs(fvar(36))=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0&&var(07)=1&&(pos y>=0||var(27)>0)&&(stateno!=[1350,1355])
triggerall=stateno!=1&&stateno!=[1000,1010]
triggerall=roundstate=2&&numenemy>0&&stateno!=41&&var(58)>0
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=(ctrl||(stateno=[0,40])||(stateno=[100,106]))&&statetype!=A
triggerall=stateno!=[1000,1099]
triggerall=!(enemynear,var(26)!=0&&(enemynear,stateno=[1000,1099]))
trigger1=ifelse(enemy,fvar(38)<=1,(random=[950,1000]),ifelse(enemy,fvar(38)=2,(random=[900,1000]),ifelse(enemy,fvar(38)=3,(random=[850,1000]),random=[840,1000])))
trigger1=var(34)>=1000
trigger1=p2dist y=[-50,20]
trigger1=abs(p2dist x)!=[-320,320]
value=1007
ctrl=0
[State *]
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=var(01)>=100
triggerall=fvar(00)<=0
triggerall=abs(fvar(36))=0
triggerall=(enemy,stateno!=[190,199])&&var(07)=1
triggerall=stateno!=1&&stateno!=[1000,1010]
triggerall=enemy,stateno!=105
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0&&(pos y>=0||var(27)>0)&&(stateno!=[1350,1355])
triggerall=roundstate=2&&numenemy>0&&stateno!=41&&var(58)>0
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=(ctrl||(stateno=[0,40])||(stateno=[100,106]))&&statetype!=A
triggerall=stateno!=[1000,1099]
triggerall=!(enemynear,var(26)!=0&&(enemynear,stateno=[1000,1099]))
trigger1=ifelse(enemy,fvar(38)<=1,(random=[980,1000]),ifelse(enemy,fvar(38)=2,(random=[950,1000]),ifelse(enemy,fvar(38)=3,(random=[940,1000]),random=[920,1000])))
trigger1=var(04)=01;&&var(34)>=1000;dagger
trigger1=abs(p2dist x)>=240
trigger1=abs(p2dist y)<=50
trigger2=ifelse(enemy,fvar(38)<=1,(random=[980,1000]),ifelse(enemy,fvar(38)=2,(random=[950,1000]),ifelse(enemy,fvar(38)=3,(random=[940,1000]),random=[920,1000])))
trigger2=var(04)=02;&&var(34)>=850;water
trigger2=p2dist y>=20
trigger2=abs(p2dist x)>=70
trigger3=ifelse(enemy,fvar(38)<=1,(random=[980,1000]),ifelse(enemy,fvar(38)=2,(random=[950,1000]),ifelse(enemy,fvar(38)=3,(random=[940,1000]),random=[920,1000])))
trigger3=var(04)=03;&&var(34)>=750;cross
trigger3=abs(p2dist x)<=100
trigger4=ifelse(enemy,fvar(38)<=1,(random=[980,1000]),ifelse(enemy,fvar(38)=2,(random=[950,1000]),ifelse(enemy,fvar(38)=3,(random=[940,1000]),random=[920,1000])))
trigger4=var(04)=04;&&var(34)>=500;axe
trigger4=abs(p2dist x)>=70
trigger4=p2dist y>-90
trigger5=ifelse(enemy,fvar(38)<=1,(random=[980,1000]),ifelse(enemy,fvar(38)=2,(random=[950,1000]),ifelse(enemy,fvar(38)=3,(random=[940,1000]),random=[920,1000])))
trigger5=var(04)=05&&numhelper(265)=0;&&var(34)>=800;bible
trigger5=screenpos x=[0,320]
trigger5=abs(p2dist x)>=100
trigger5=p2dist y<=-40
value=1000
ctrl=0
[State *]
type=varset
triggerall=numenemy
triggerall=authorname="Boss"
triggerall=var(51)!=2
triggerall=fvar(39)=0&&enemy,fvar(39)=0
triggerall=sysvar(2)=0&&var(07)=0
triggerall=(stateno!=[1000,1099])
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1
triggerall=var(58)>0
triggerall=roundstate>=2
trigger1=var(58)>0&&life<=floor(var(35)*0.5)
sysvar(2)=1
[State *];agunea
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=authorname="Boss"
triggerall=var(58)>0
triggerall=sysvar(2)=0
triggerall=var(51)!=2
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=roundstate>=2
triggerall=numhelper(1570)=0
triggerall=var(34)>=ifelse(var(58)<=0,300,600)
triggerall=stateno!=1&&stateno!=1009&&abs(var(19))!=4
;triggerall=statetype!=A
triggerall=(stateno!=[207,208])&&(stateno!=[407,408])&&(stateno!=[607,608])
triggerall=life>0&&abs(fvar(36))=0&&(enemy,life>0||enemy,var(52)>0)
triggerall=numhelper(1360)=0&&numhelper(1365)=0
triggerall=(ctrl&&stateno<=50)||(ctrl&&stateno=[100,104])||stateno=105||(stateno=102&&time>=32)||((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
triggerall=screenpos x=[0,320]
triggerall=abs(p2dist y)<=50
trigger1=abs(p2dist x)=[176,320]
trigger1=random>=ifelse(enemy,fvar(38)=1,990,ifelse(enemy,fvar(38)=2,960,ifelse(enemy,fvar(38)=3,940,920)))
;trigger2=(enemy,fvar(38)!=[1,2])&&((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
;trigger2=p2dist y=[-40,50]
value=1550
[State *];boss flame whip
type=changestate
triggerall=numenemy
triggerall=authorname="Boss"
triggerall=var(58)>0
triggerall=var(51)!=2
triggerall=fvar(39)=0&&enemy,fvar(39)=0
triggerall=sysvar(2)=1;&&abs(var(19))!=2
triggerall=var(07)=0&&(pos y>=0||stateno=0)
triggerall=(stateno!=[1000,1099])
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&(stateno!=[1350,1355])
triggerall=roundstate>=2
trigger1=(stateno<=50)||stateno=52||(stateno=[100,105])
trigger1=var(58)>0&&life<=floor(var(35)*0.5)
value=1349
ctrl=0
[State *];whip dash
type=changestate
triggerall=numenemy
triggerall=sysvar(2)=0
triggerall=stateno!=1&&stateno!=1009
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=life>0&&(enemy,life>0||enemy,var(52)>0)
triggerall=roundstate=2&&numenemy>0&&stateno!=41
triggerall=var(58)>0;&&abs(var(19))!=1
;triggerall=enemy,stateno!=[105,106]
;triggerall=(var(58)>0&&enemynear,var(26)=0)||(var(58)<=0)
triggerall=stateno!=[1100,1149]
triggerall=stateno!=[1000,1099]
triggerall=!(enemynear,var(26)!=0&&(enemynear,stateno=[1000,1099]))
triggerall=(ctrl||(stateno=[0,60])||(stateno=[100,106])||((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19]))
triggerall=var(34)>=100
triggerall=abs(fvar(36))=0
triggerall=enemynear,var(26)=0
triggerall=var(06)<=0||(prevstateno=49&&time=10&&random>=500)
triggerall=abs(p2dist x)<=100
triggerall=abs(p2dist y)<=50
trigger1=random>=ifelse(enemy,fvar(38)<=1,980,ifelse(enemy,fvar(38)=2,930,ifelse(enemy,fvar(38)=3,880,800)))/(1+((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19]))
trigger2=enemy,p2dist x>=0
trigger2=(enemynear,stateno=[890,899])||(enemynear,stateno=[1100,2999])
trigger3=prevstateno=49||anim%10000=49
trigger3=p2dist y=[-30,40]
trigger4=(var(49)=-1&&facing=-1)||(var(49)=1&&facing=1)
trigger5=(enemy,fvar(38)!=[1,2])&&((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
trigger5=p2dist y=[-40,50]
value=1100;+((enemy,fvar(38)>=4&&p2dist y>20)*1)+((enemy,fvar(38)>=4&&p2dist y<-10)*2)
ctrl=0
;/AI---------------
[State *];agunea
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=var(51)!=2
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=roundstate>=2
triggerall=numhelper(1570)=0
triggerall=var(34)>=ifelse(var(58)<=0,200,400)
triggerall=stateno!=1&&stateno!=1009
;triggerall=statetype!=A
triggerall=(stateno!=[207,208])&&(stateno!=[407,408])&&(stateno!=[607,608])
triggerall=(command="HCF+a"||(abs(var(56))=48))
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&abs(p2dist x)<200)
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)
trigger2=stateno=200&&animelem=10,>=0&&movecontact
trigger3=stateno=400&&animelem=10,>=0&&movecontact
trigger4=stateno=600&&animelem=10,>=0&&movecontact
trigger5=stateno=205&&animelem=8,>=0&&movecontact
trigger6=stateno=405&&animelem=8,>=0&&movecontact
trigger7=stateno=605&&animelem=8,>=0&&movecontact
value=1550
[State *];agunea
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=var(51)!=2
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=roundstate>=2
triggerall=numhelper(1570)=0
triggerall=var(34)>=ifelse(var(58)<=0,200,400)
triggerall=stateno!=1&&stateno!=1009
triggerall=statetype!=A
triggerall=(stateno!=[207,208])&&(stateno!=[407,408])&&(stateno!=[607,608])
triggerall=(command="HCF+b"||(abs(var(56))=49))
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&abs(p2dist x)<200)
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)
trigger2=stateno=200&&animelem=10,>=0&&movecontact
trigger3=stateno=400&&animelem=10,>=0&&movecontact
trigger4=stateno=600&&animelem=10,>=0&&movecontact
trigger5=stateno=205&&animelem=8,>=0&&movecontact
trigger6=stateno=405&&animelem=8,>=0&&movecontact
trigger7=stateno=605&&animelem=8,>=0&&movecontact
value=1600
[State *];aura blast
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=var(51)=2
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=roundstate>=2
triggerall=var(34)>=ifelse(var(58)<=0,250,500)
triggerall=stateno!=1&&stateno!=1009
triggerall=(stateno!=[207,208])&&(stateno!=[407,408])&&(stateno!=[607,608])
triggerall=(command="HCF+a"||command="HCF+b"||(abs(var(56))=[48,49]))
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&abs(p2dist x)<32)
triggerall=numhelper(1360)=0&&numhelper(1365)=0
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)
trigger2=stateno=200&&animelem=10,>=0&&movecontact
trigger3=stateno=400&&animelem=10,>=0&&movecontact
trigger4=stateno=600&&animelem=10,>=0&&movecontact
trigger5=stateno=205&&animelem=8,>=0&&movecontact
trigger6=stateno=405&&animelem=8,>=0&&movecontact
trigger7=stateno=605&&animelem=8,>=0&&movecontact
value=1500
[State *]
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=var(01)>=100
triggerall=fvar(00)<=0
triggerall=fvar(01)>0
triggerall=(enemy,stateno!=[190,199])&&statetype!=A
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&ifelse(authorname="boss"&&enemy,fvar(39)=0,var(07)=1,enemy,fvar(38)>=3)&&(stateno!=[1000,1099]))
triggerall=roundstate>=2
triggerall=!(command="UDF+a"||command="UDF+b"||(abs(var(56))=[28,29]))
triggerall=command="itemcrash"
triggerall=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)||((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
trigger1=var(04)=01;&&var(34)>=300;Dagger
trigger2=var(04)=02&&numhelper(1010)=0;&&var(34)>=850;Water
trigger3=var(04)=03&&numhelper(1020)=0;&&var(34)>=750;Cross
trigger4=var(04)=04&&numhelper(1030)=0;&&var(34)>=550;Axe
trigger5=var(04)=05&&numhelper(1040)=0;&&var(34)>=850;Bible
trigger6=var(04)=06&&numhelper(1050)=0;&&var(34)>=300;Rebound Stone
value=1000
ctrl=0
[State *];item crash possibility
type=null
triggerall=numenemy
trigger1=var(58)<=0||(var(58)>0&&authorname="CvF")||(var(58)>0&&authorname="boss"&&var(07)=1)
trigger1=fvar(01)!=1
trigger1=var(01)>=100
trigger1=0&&fvar(01):=1
trigger2=fvar(01)=1
trigger2=var(01)<100||(var(58)>0&&authorname="boss"&&var(07)<1)
trigger2=0&&fvar(01):=0
ignorehitpause=1
[State *];whip dash
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=roundstate=2
triggerall=!(command="itemcrash")
triggerall=(command="UDF+a"||(abs(var(56))=28));((abs(var(56))=[28,29])||(abs(var(56))=[38,39]))
triggerall=var(34)>=100
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&abs(p2dist x)<80&&enemynear,var(26)=0&&(stateno!=[1000,1099]))
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)||((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
value=1100;+((p2dist y>40)*1)+((p2dist y<-10)*2)
ctrl=0
[State *];whip dash
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=roundstate=2
triggerall=!(command="itemcrash")
triggerall=(command="UDF+b"||(abs(var(56))=29));((abs(var(56))=[28,29])||(abs(var(56))=[38,39]))
triggerall=var(34)>=120
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&abs(p2dist x)<80&&enemynear,var(26)=0&&(stateno!=[1000,1099]))
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)||((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
value=1150;+((p2dist y>40)*1)+((p2dist y<-10)*2)
ctrl=0
[State *];flame whip
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&abs(p2dist x)<88)
triggerall=(command="FF+a"||command="FF+b")&&(abs(var(56))!=[38,39]);||(abs(var(56))=[68,69])
triggerall=!(command="HCF+a"||command="HCF+b"||(abs(var(56))=[48,49]))
triggerall=!(command="UDF+a"||command="UDF+b"||(abs(var(56))=[28,29]));((abs(var(56))=[28,29])||(abs(var(56))=[38,39]))
;triggerall=!(command="DF+a"||command="DF+b");(abs(var(56))=[58,59])
triggerall=command!="itemcrash"
triggerall=var(34)>=150
triggerall=roundstate=2
triggerall=stateno!=1&&stateno!=1009&&stateno!=1200;&&anim%10000!=49
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)||((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
trigger2=stateno=200&&animelem=8,>=0&&movecontact
trigger3=stateno=400&&animelem=8,>=0&&movecontact
trigger4=stateno=600&&animelem=8,>=0&&movecontact
value=1200;1200+((statetype=A)*20);206+((statetype=A)*400)
ctrl=0
[State *];kick
type=changestate
triggerall=numenemy
triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=stateno!=1&&stateno!=1009
triggerall=!(prevstateno=1400&&statetype=A)
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=roundstate>=2;&&statetype!=A
triggerall=(command!="itemcrash")
triggerall=!(command="HCF+a"||command="HCF+b"||(abs(var(56))=[48,49]))
triggerall=!(command="UDF+a"||command="UDF+b"||(abs(var(56))=[28,29]));((abs(var(56))=[28,29])||(abs(var(56))=[38,39]))
triggerall=!(command="FF+a"||command="FF+b");(abs(var(56))=[68,69])
triggerall=(command="DF+a"||command="DF+b");(abs(var(56))=[58,59])
triggerall=var(34)>=80
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&abs(p2dist x)<56&&(stateno!=[1000,1099]))
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)||((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
value=1400
ctrl=0
[State *];subweapon
type=changestate
triggerall=numenemy
triggerall=sysvar(2)=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&(stateno!=[1000,1099]))
triggerall=roundstate=2
triggerall=stateno!=1&&stateno!=1009
triggerall=(command!="UDF+a")&&(command!="UDF+b")&&(command!="itemcrash")&&(command="attack"||command="attack2")&&command="holdup";&&anim%10000!=49;&&command!="holddown"
triggerall=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)||((movecontact||((abs(var(56))%10)/1)>=8&&time<1)&&(stateno=[200,699])&&stateno%200=[0,19])
trigger1=var(04)=01&&numhelper(220)<=floor(fvar(32))&&var(34)>=25;Dagger
trigger2=var(04)=02&&numhelper(230)<=0&&var(34)>=50;Water
trigger3=var(04)=03&&numhelper(240)<=floor(fvar(32))&&var(34)>=100;Cross
trigger4=var(04)=04&&numhelper(250)<=floor(fvar(32))&&var(34)>=30;Axe
trigger5=var(04)=05&&numhelper(260)<=floor(fvar(32))&&var(34)>=35;bible
trigger6=var(04)=06&&numhelper(270)<=floor(1+(fvar(32)*2))&&var(34)>=25;Rebound S
value=250+((statetype!=A&&command="holddown")*200)+((statetype=A)*400)
anim=250+((command="attack2")||(var(58)>0&&random>515))+((statetype!=A&&command="holddown")*200)+((statetype=A)*400)+(var(51)*10000)
ctrl=0
[State *];whip
type=changestate
triggerall=numenemy
triggerall=sysvar(2)=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=roundstate=2
triggerall=stateno!=1&&stateno!=1009
triggerall=(command!="UDF+a")&&(command!="UDF+b")&&(command!="itemcrash");&&anim%10000!=49
triggerall=(command="attack"||command="attack2")
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&abs(p2dist x)<64&&(p2dist y=[-40,20]))
trigger1=(ctrl&&(stateno<=50||stateno=[100,104]))||stateno=52||stateno=105||(stateno=102&&anim%10000=101)
trigger2=stateno=200&&command="holddown"&&animelem=5,>=0&&movecontact
trigger3=stateno=600&&statetype=S&&command="holddown"&&movecontact&&animelem=5,>=0&&movecontact
value=200+(((statetype!=A||(stateno=114&&pos y>=0&&var(23)<=0))&&command="holddown")*200)+(((statetype=A||stateno=114)&&(pos y<0||var(23)>0))*400)
ctrl=0
[State Flame Whip Mode]
type=changestate
triggerall=numenemy
;triggerall=var(57)=0
triggerall=sysvar(2)=0
triggerall=var(51)!=2
triggerall=!(authorname="boss")
;triggerall=var(07)<1
triggerall=stateno!=1&&stateno!=1009
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&var(14)<=0
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&var(07)<=0&&life>0&&life<(var(35)/2)&&(enemy,life>0||enemy,var(52)>0)&&(stateno!=[1000,1099])&&(fvar(39)!=0||enemy,fvar(39)!=0))
triggerall=roundstate>=2
triggerall=command="R"
triggerall=var(34)>=ifelse(var(07)=0,50,0)
trigger1=ctrl&&statetype!=A
trigger2=stateno=100
value=1305;0+((var(58)<=0)*5)
ctrl=0
[State *]
type=changestate
triggerall=numenemy
triggerall=anim%10000!=105
triggerall=sysvar(2)=0
triggerall=fvar(09)>=1&&enemy,fvar(09)>=1&&floor((var(16)%10)/1)=0
triggerall=command="L"&&command!="holdup"&&command!="holddown"
triggerall=(statetype!=A||(statetype=A&&var(23)>0))&&(stateno!=[53,59])&&roundstate=2
triggerall=numhelper(54000)&&numhelper(54001)
triggerall=stateno!=1&&stateno!=22&&stateno!=102&&(stateno!=[105,119])&&(stateno!=[890,899])&&(stateno!=[1000,1099])&&stateno!=30&&stateno!=32
triggerall=var(58)<=0||(var(58)>0&&abs(fvar(36))=0&&life>0&&(enemy,life>0||enemy,var(52)>0)&&var(49)=0&&stateno<1000&&var(23)=0)
trigger1=(statetype=S||(statetype=A&&var(23)>0&&(selfanimexist(9000040+var(51)))))&&((ctrl&&stateno<=50)||stateno=31||stateno=52||(ctrl&&stateno=[100,104])||(stateno=102&&anim%10000=101)||stateno=31)
trigger2=(stateno=200||stateno=400||(stateno=600&&(anim%10000<600||var(23)>0)))&&animelemtime(2)>=0
trigger3=(stateno=210||stateno=410||(stateno=610&&(anim%10000<600||var(23)>0)))&&animelemtime(2)>=0
trigger4=(stateno=250||stateno=450||(stateno=650&&(anim%10000<600||var(23)>0)))&&animelemtime(2)>=0
trigger5=(anim%10000=[1200,1219])&&animelemtime(7)>=0
value=105
ctrl=0
[Statedef -2]
[State *]
type=varset
trigger1=numpartner>0
var(52)=partner,life
[State *]
type=assertspecial
triggerall=numenemy
triggerall=!numhelper(64100);no survival score
triggerall=!enemy,numhelper(64100);no survival score for enemy
triggerall=fvar(39)=0&&enemy,fvar(39)=0
trigger1=roundstate=2
trigger1=var(50)=0&&life>0;player didnt grab orb yet, has life so not dead
trigger2=!numpartner;single player
trigger2=life<=0;dead
trigger2=fvar(33)>=0&&var(58)<=0;has lives, isnt ai controlled
trigger3=numpartner;coop
trigger3=life<=0&&var(52)<=0;both dead
trigger3=(fvar(33)>=0&&var(58)<=0)||(partner,fvar(33)>=0&&partner,var(58)<=0);has lives, isnt ai controlled/partner has lives, isnt ai controlled
flag=NoKO
ignorehitpause=1
[State *]
type=null
triggerall=numenemy
triggerall=var(58)<=0
;triggerall=command="holdstart"
triggerall=stateno=[5900,5901]
trigger1=(palno=[0,6])&&var(51)!=0
trigger1=0&&var(51):=0
trigger2=((palno=[7,8])||(palno=[10,11]))&&var(51)!=1
trigger2=0&&var(51):=1
trigger3=(palno=9)&&var(51)!=2
trigger3=0&&var(51):=2
trigger4=(palno=12)&&var(51)!=3
trigger4=0&&var(51):=3
[State *]
type=null
triggerall=numenemy
triggerall=var(58)<=0
triggerall=stateno=[5900,5901]
triggerall=enemy,fvar(39)=0||(enemy,fvar(39)!=0&&authorname="boss")
triggerall=name="Richter"
trigger1=(palno=[0,6])&&var(51)!=0
trigger1=0&&var(51):=0
trigger2=((palno=[7,8])||(palno=[10,11]))&&var(51)!=1
trigger2=0&&var(51):=1
trigger3=(palno=9)&&var(51)!=2
trigger3=0&&var(51):=2
trigger4=(palno=12)&&var(51)!=3
trigger4=0&&var(51):=3
[State *]
type=null
triggerall=numenemy
triggerall=stateno=[5900,5901]
;triggerall=enemy,fvar(14)<1
trigger1=palno>=7&&palno!=9&&palno!=12&&var(51)!=1&&(var(58)<=0||(var(58)>0&&enemy,name!="Richter"&&enemy,name!="Maria"))
trigger1=0&&var(51):=1
trigger2=var(58)>0&&var(51)!=1&&var(51)!=2
trigger2=enemy,name="Maria"&&enemy,var(51)=1
trigger2=0&&var(51):=1
trigger3=enemy,fvar(14)<1
trigger3=var(58)>0&&var(51)!=1&&var(51)!=2
trigger3=enemy(0),name="Richter"&&enemy(0),var(51)=0
trigger3=0&&var(51):=1
trigger4=var(58)>0&&var(51)!=0&&var(51)!=2
trigger4=enemy(0),var(51)=1&&enemy(0),name="Richter"
trigger4=0&&var(51):=0
;trigger5=!(authorname="boss")
trigger5=enemy,fvar(14)<1
trigger5=numenemy>1&&var(58)>0&&var(51)!=3
trigger5=enemy(0),name="Richter"&&enemy(0),var(51)!=3
trigger5=enemy(1),name="Richter"&&enemy(1),var(51)!=3
trigger5=0&&var(51):=3
trigger6=var(58)>0&&(palno=9)&&var(51)!=ifelse(abs(var(59))=76&&authorname="boss",1,2)
trigger6=0&&var(51):=ifelse(abs(var(59))=76&&authorname="boss",1,2)
trigger7=var(58)>0&&(palno=12)&&var(51)!=3;ifelse(abs(var(59))=76&&authorname="boss",1,3)
trigger7=0&&var(51):=3;ifelse(abs(var(59))=76&&authorname="boss",1,3)
[State *]
type=null
triggerall=stateno=[5900,5901]
triggerall=roundstate<2
triggerall=numenemy
triggerall=enemy,fvar(39)!=0
triggerall=enemy,var(58)<=0
triggerall=!(authorname="boss")
triggerall=palno!=9&&palno!=12
triggerall=name="Richter"
trigger1=0&&var(51):=random%4
[State *]
type=null
triggerall=stateno=[5900,5901]
triggerall=name="Richter"&&!(authorname="boss")
trigger1=palno=9&&var(51)!=2
trigger1=0&&var(51):=2
trigger2=palno=12&&var(51)!=3
trigger2=0&&var(51):=3
[State *];subweapon
type=null
triggerall=0
triggerall=numenemy
triggerall=name="Richter"
triggerall=roundstate=2&&life>0&&sysvar(2)=0&&fvar(09)>=1&&enemy,fvar(09)>=1
triggerall=(stateno!=[250,259])&&(stateno!=[450,459])&&(stateno!=[650,659])&&(stateno!=[1000,1099])
triggerall=var(58)<=0&&movetype!=H
trigger1=command="D"
trigger1=0&&var(04):=var(04)+ifelse(command!="holdup",1,-1)
trigger2=var(04)>06||var(04)<01
trigger2=0&&var(04):=ifelse(command="D"&&command="holdup",06,01)
[State *]
type=null
trigger1=var(51)!=2
trigger1=var(42)!=2
trigger1=0&&var(42):=2
trigger2=var(51)=2
trigger2=var(42)!=14
trigger2=0&&var(42):=14
trigger3=var(06)>0
trigger3=0&&var(06):=var(06)-1
[State *]
type=null
triggerall=0
triggerall=numenemy
triggerall=!(authorname="boss")
trigger1=movetype!=H&&var(06)>0
trigger1=(stateno!=[120,199])&&(stateno!=[49000,49999])&&(stateno!=1000)&&(stateno!=[4990,5300])&&(stateno!=[49800,49809])
trigger1=var(07)=1&&gametime%4=0
trigger1=0&&var(06):=var(06)-3
[State *]
type=null
triggerall=numenemy
triggerall=var(07)>=1
trigger1=life<=0||roundstate!=2||(var(34)<=0&&!(authorname="boss"))||(sysvar(2)>0&&var(58)<=0&&!(authorname="boss"))
trigger1=0&&var(07):=0
[State *]
type=attackmulset
trigger1=var(58)<=0&&var(07)!=0
value=(1.00+(var(37)*0.01)+((var(09)!=0)*0.25))/ifelse(var(13)<0,2.0,1.0)
ignorehitpause=1
[State PoweR]
type=varadd
triggerall=numenemy
triggerall=!numhelper(7710)
triggerall=fvar(00)>=0
triggerall=var(01)>=0
triggerall=var(23)<=0
triggerall=var(14)>=0&&var(07)<=0&&var(25)=0
triggerall=stateno!=[1000,1009]
triggerall=sysvar(2)=0
triggerall=(movetype=I||stateno=[40,59])&&var(34)<var(36)&&roundstate=2
triggerall=(stateno!=[120,199])&&(stateno!=[200,5999])
triggerall=(stateno!=[250,259])&&(stateno!=[450,459])&&(stateno!=[650,659])&&(stateno!=[900,999])
triggerall=(var(58)<=0&&gametime%(4+((var(25)>0)*12)+((var(01)<25)*1)+((var(01)<50)*1)+((var(01)<75)*1))=0)||(var(58)>0&&gametime%4=0)
triggerall=!(authorname="boss")
trigger1=1
var(34)=((2+((fvar(37)>=120&&var(58)<=0)*6))+floor((var(58)>0&&!ishometeam)*4));/(numpartner+1)
ignorehitpause=1
[State *]
type=helper
triggerall=numenemy
triggerall=!numhelper(187)
triggerall=enemy,name!="intro"
triggerall=!(authorname="boss")
;triggerall=!(enemy,authorname="bonus")
triggerall=var(51)!=2
triggerall=roundstate=2&&enemy,var(52)=0&&life>0
triggerall=(!(enemy,authorname="bonus")&&enemy,life<=0)||(enemy,authorname="bonus")
trigger1=enemy,name!="Shaft";&&enemy,name!="Dracula"&&enemy,name!="EvilSoma"&&enemy,name!="Carmilla"&&enemy,name!="Olrox"
trigger1=ifelse(enemy,authorname="CvF",enemy,stateno=4998,enemy,stateno=4993)
trigger2=fvar(39)=0
trigger2=ifelse(fvar(14)<=0,enemy,stateno=2050&&enemy,time<4,enemy,stateno=4995)
trigger2=enemy,name="Shaft"&&enemy,var(04)=1;+floor(fvar(38)>=3)
trigger3=enemy,authorname="bonus"&&enemy,var(40)=7
;trigger3=fvar(39)=0
;trigger3=enemy,name="Dracula"&&enemy,stateno=4993
;trigger4=fvar(39)=0
;trigger4=enemy,name="EvilSoma"&&enemy,stateno=4993
;trigger5=fvar(39)=0
;trigger5=enemy,name="Carmilla"&&enemy,stateno=4993
;trigger6=fvar(39)=0
;trigger6=enemy,name="Olrox"&&enemy,stateno=4993
helpertype=normal
name="DeathVoice"
id=187
pos=0,0
postype=p1
facing=1
stateno=187
ownpal=1
pausemovetime=99999999999999999999999
supermovetime=99999999999999999999999
ignorehitpause=1
;=====================
;=====stats&stuff=====
;=====================
[State *];player richter
type=varset
triggerall=numenemy
triggerall=!(authorname="boss")
triggerall=var(51)!=2
trigger1=enemy,numhelper(64000)
trigger2=life>0
trigger2=var(35)<=1
var(35)=1300+((enemy,fvar(38)>=3)*250)+(((enemy,fvar(39)!=0)&&(enemy,roundsexisted>0))*((enemy,roundsexisted)*25))
;var(35)=cond(numpartner&&id<partner,id,10,1300+((enemy,fvar(38)>=3)*250)+(((enemy,fvar(39)!=0)&&(enemy,roundsexisted>0))*((enemy,roundsexisted)*25)))
ignorehitpause=1
[State *];boss richter
type=varset
triggerall=numenemy
triggerall=(authorname="boss")
triggerall=var(51)!=2
trigger1=enemy,numhelper(64000)
trigger2=life>0
trigger2=var(35)<=1
var(35)=floor(1500+((-2+enemy,fvar(38))*(192+(random%32))))+(((enemy,fvar(39)!=0)&&(enemy,roundsexisted>0))*((enemy,roundsexisted)*25))
ignorehitpause=1
[State *];hanz
type=varset
triggerall=numenemy
triggerall=var(51)=2
trigger1=enemy,numhelper(64000)
trigger2=life>0
trigger2=var(35)<=1
var(35)=950+((enemy,fvar(38)=1&&var(58)>0)*350)+((enemy,fvar(38)>1&&var(58)>0)*500)+(((enemy,fvar(39)!=0)&&(enemy,roundsexisted>0))*((enemy,roundsexisted)*25))
ignorehitpause=1
[State *]
type=lifeadd
triggerall=numenemy
triggerall=stateno!=[49800,49899]
trigger1=(stateno=[190,199])||(stateno=[49000,49999])
trigger1=life>0
trigger1=var(35)>0&&(life<var(35))
trigger2=enemy,numhelper(64000)
value=var(35)
ignorehitpause=1
[State *]
type=varset
triggerall=numenemy
trigger1=var(36)<=0
trigger1=life>0
var(36)=floor(ifelse(var(58)>0||authorname="boss",2000,1000+((var(51)=2)*250)))
ignorehitpause=1
[State *]
type=varset
trigger1=var(35)>9999
var(35)=9999
ignorehitpause=1
[State *]
type=varset
trigger1=var(35)>lifemax
var(35)=lifemax
ignorehitpause=1
[State *]
type=varset
trigger1=var(36)>9999
var(36)=9999
ignorehitpause=1
[State *]
type=varset
trigger1=var(37)>256
var(37)=256
ignorehitpause=1
[State *]
type=varset
trigger1=var(38)>256
var(38)=256
ignorehitpause=1
[State *]
type=varset
trigger1=abs(var(21))>9999
var(21)=ifelse(var(21)>0,9999,-9999)
ignorehitpause=1
[State *]
type=lifeset
trigger1=life>var(35)
value=floor(var(35))
ignorehitpause=1
[State str boost hanz]
type=varset
trigger1=var(37)<64&&var(51)=2
var(37)=64
ignorehitpause=1
[State *]
type=varset
trigger1=numpartner
trigger1=var(36)<partner,var(36)
var(36)=floor(partner,var(36))
ignorehitpause=1
[State *];elemental weaknesses boss richter
type=null
triggerall=numenemy
triggerall=authorname="boss"
triggerall=var(51)!=2
triggerall=roundstate<2||var(39)=0||fvar(09)=0
trigger1=0&&var(39):=73554
trigger2=0&&fvar(24):=((0.93-((var(58)>0&&enemy,fvar(38)>2)*0.05))/ifelse(var(58)>0||authorname="boss",1.5+(numenemy-1)*0.25,1.00))*(1.0-(var(38)*0.0039))
trigger3=0&&fvar(25):=((0.98-((var(58)>0&&enemy,fvar(38)>2)*0.05))/ifelse(var(58)>0||authorname="boss",1.5+(numenemy-1)*0.25,1.00))*(1.0-(var(38)*0.0039))
trigger4=0&&fvar(26):=((1.00+((var(58)>0&&enemy,fvar(38)>2)*0.00))/ifelse(var(58)>0||authorname="boss",1+(numenemy-1)*0.33,1.00))*(1.0-(var(38)*0.0039));unique, leave alone
pausemovetime=99999999999999999999999
supermovetime=99999999999999999999999
[State *];elemental weaknesses richter
type=null
triggerall=numenemy
triggerall=!(authorname="boss")
triggerall=var(51)!=2
triggerall=roundstate<2||var(39)=0||fvar(09)=0
trigger1=0&&var(39):=64555
trigger2=0&&fvar(24):=((0.91-((var(58)>0&&enemy,fvar(38)>2)*0.05))/ifelse(var(58)>0||authorname="boss",1+(numenemy-1)*0.33,1.00))*(1.0-(var(38)*0.0039))
trigger3=0&&fvar(25):=((1.15-((var(58)>0&&enemy,fvar(38)>2)*0.10))/ifelse(var(58)>0||authorname="boss",1+(numenemy-1)*0.33,1.00))*(1.0-(var(38)*0.0039))
trigger4=0&&fvar(26):=((1.00+((var(58)>0&&enemy,fvar(38)>2)*0.00))/ifelse(var(58)>0||authorname="boss",1+(numenemy-1)*0.33,1.00))*(1.0-(var(38)*0.0039));unique, leave alone
pausemovetime=99999999999999999999999
supermovetime=99999999999999999999999
[State *];elemental weaknesses hanz
type=null
triggerall=numenemy
triggerall=var(51)=2
triggerall=roundstate<2||var(39)=0||fvar(09)=0
trigger1=0&&var(39):=64555
trigger2=0&&fvar(24):=((1.50-((var(58)>0&&enemy,fvar(38)=2)*0.15)-((var(58)>0&&enemy,fvar(38)=3)*0.34)-((var(58)>0&&enemy,fvar(38)>3)*0.60))/ifelse(var(58)>0||authorname="boss",1+(numenemy-1)*0.33,1.00))*(1.0-(var(38)*0.0039))
trigger3=0&&fvar(25):=((1.30+((var(58)>0&&enemy,fvar(38)=2)*0.10)-((var(58)>0&&enemy,fvar(38)=3)*0.56)-((var(58)>0&&enemy,fvar(38)>3)*0.80))/ifelse(var(58)>0||authorname="boss",1+(numenemy-1)*0.33,1.00))*(1.0-(var(38)*0.0039))
trigger4=0&&fvar(26):=((1.00+((var(58)>0&&enemy,fvar(38)=2)*0.00)+((var(58)>0&&enemy,fvar(38)=3)*0.00)+((var(58)>0&&enemy,fvar(38)>3)*0.00))/ifelse(var(58)>0||authorname="boss",1+(numenemy-1)*0.33,1.00))*(1.0-(var(38)*0.0039));unique, leave alone
pausemovetime=99999999999999999999999
supermovetime=99999999999999999999999
[State *]
type=varset
trigger1=var(20)!=0&&var(24)=0
var(20)=0
[State *]
type=varset
trigger1=var(20)!=1&&var(24)>0
var(20)=1
[State *]
type=varset
trigger1=sysvar(4)!=40-((statetype=C)*20)
sysvar(4)=40-((statetype=C)*20)
[State *]
type=varset
triggerall=numenemy
trigger1=var(58)>0&&var(37)!=-16-floor((enemy,fvar(38))*4)
var(37)=-16-floor((enemy,fvar(38))*4)
ignorehitpause=1
;=====================
;====/stats&stuff=====
;=====================
;=========================================================
;============= intro, winpose, extra shit ================
;=========================================================
[State *]
type=assertspecial
trigger1=numenemy
trigger1=authorname="Boss"&&enemy,fvar(39)<=0&&stateno=190&&abs(var(59))=76
flag=Invisible
ignorehitpause=1
[State running]
type=playsnd
triggerall=anim%10000=100
trigger1=animelem=2,=0
trigger2=animelem=6,=0
value=F0,87
pan=-floor((160-screenpos x)*3)*facing
volume=floor(-128-abs(pos x*0.01))
volumescale=50-(abs((screenpos x-160)*0.185)+abs((screenpos y-120)*0.185))
ignorehitpause=1
[State hurt voices]
type=playsnd
trigger1=(stateno=[4990,4992])&&time=1&&life>0&&random>=300&&(prevstateno!=[4990,4999])
value=S1+((var(51)=2)*4),0+((var(51)!=2)*(random%5))
channel=0
pan=-floor((160-screenpos x)*3)*facing
volume=floor(256-(abs((screenpos x-160)*0.40)+abs((screenpos y-120)*0.40)))
volumescale=100-(abs((screenpos x-160)*0.185)+abs((screenpos y-120)*0.185))
ignorehitpause=1
[State victory voice]
type=playsnd
triggerall=stateno=180
triggerall=var(51)!=2
triggerall=numenemy
trigger1=(prevstateno!=[170,199])&&(prevstateno!=[49000,49999])
trigger1=time=1&&((enemy,name!="Dracula"&&enemy,name!="EvilSoma"&&fvar(39)=0&&enemy,var(05)<1)||fvar(39)!=0)
value=S2,ifelse(authorname="boss",ifelse(random>515,0+random%3,6),11+random%3);10+(random>515))
channel=0
pan=-floor((160-screenpos x)*3)*facing
volume=256
volumescale=100
ignorehitpause=1
[State victory voice extra timer]
type=assertspecial
triggerall=stateno=180&&var(51)!=2
trigger1=time<330&&var(58)>0
flag=RoundNotOver
ignorehitpause=1
[State *]
type=explod
triggerall=time>1
triggerall=anim%10000=49
triggerall=var(07)=1
trigger1=animelemtime(3)>=0&&vel y<0
;trigger1=time%2=0
anim=F250
id=1100
pos=8,-48
postype=p1
vel=0,-0.4
random=8,16
removetime=-2
bindtime=4
sprpriority=5
facing=ifelse(random>515,1,-1)
trans=addalpha
alpha=128,256
ownpal=1
ignorehitpause=1
[State *]
type=explod
triggerall=frontedgebodydist>0
trigger1=stateno=890&&time<=16&&vel x
anim=F33
id=33
pos=7,0
postype=p1
facing=ifelse(random>515,1,-1)
vel=0,-0.4
bindtime=1
random=10,0
removetime=-2
scale=1,1
sprpriority=2
ownpal=1
ignorehitpause=1
[State *]
type=explod
triggerall=frontedgebodydist>0
trigger1=stateno=890&&time<=16&&vel x
anim=F33
id=33
pos=17,0
postype=p1
facing=ifelse(random>515,1,-1)
vel=0,-0.4
bindtime=1
random=10,0
removetime=-2
scale=1,1
sprpriority=2
ownpal=1
ignorehitpause=1
[State *]
type=playsnd
triggerall=stateno=895
triggerall=var(51)!=2
trigger1=time<=1
value=S0,5+(random>515)
channel=0
pan=-floor((160-screenpos x)*3)*facing
volume=floor(256-(abs((screenpos x-160)*0.40)+abs((screenpos y-120)*0.40)))
volumescale=100-(abs((screenpos x-160)*0.185)+abs((screenpos y-120)*0.185))
ignorehitpause=1
persistent=0
[State *]
type=playsnd
triggerall=stateno=895
triggerall=var(51)=2
trigger1=time<=1
value=S4,0+(random%3)
pan=-floor((160-screenpos x)*3)*facing
volume=floor(256-(abs((screenpos x-160)*0.40)+abs((screenpos y-120)*0.40)))
volumescale=100-(abs((screenpos x-160)*0.185)+abs((screenpos y-120)*0.185))
channel=0
ignorehitpause=1
persistent=0
[State *]
type=turn
trigger1=anim%10000=897&&animelemtime(2)=0
ignorehitpause=1
persistent=0
;intro whip rush
[State *]
type=changeanim
triggerall=enemy,stateno=[40000,40999]
trigger1=anim%10000=195
trigger1=enemy,name="Montano"&&enemy,var(46)=13
value=1100+var(51)*10000
[State *]
type=velset
triggerall=enemy,stateno=[40000,40999]
trigger1=anim%10000=1100
trigger1=animelemtime(2)=2
x=12.0
ignorehitpause=1
[State *]
type=changeanim
triggerall=enemy,stateno=[40000,40999]
triggerall=p2dist x=[-5,5]
trigger1=anim%10000=1100&&enemy,name="Montano"
value=105+var(51)*10000
[State *]
type=velset
triggerall=enemy,stateno=[40000,40999]
trigger1=anim%10000=105&&animelemtime(1)=0
x=-5.6
persistent=0
ignorehitpause=1
[State *]
type=velmul
trigger1=anim%10000=105&&animelemtime(2)>=0
trigger2=anim%10000=174
x=0.90
ignorehitpause=1
[State *]
type=changeanim
triggerall=enemy,stateno=[40000,40999]
trigger1=anim%10000=105&&!animtime
value=174+var(51)*10000
;=========================================================
;============= /intro, winpose, extra shit ===============
;=========================================================
[State *]
type=null;displaytoclipboard
trigger1=numenemy
text="psn=%d,x=%d,x=%d,%d,%d"
params=prevstateno,var(07),var(14),var(16),command="DF+a"
ignorehitpause=1
[State *]
type=displaytoclipboard
trigger1=1
text="psn=%d,%d,%f,%f,"
params=prevstateno,stateno,fvar(33),0
;,abs((pos x-enemynear,pos x)*-facing),abs(enemynear,const(size.attack.dist))+abs(const(size.attack.dist))+abs((enemynear,name="Remilia"&&enemynear,stateno=220)*enemynear,vel x*2.25)
ignorehitpause=1
[State *]
type=null;displaytoclipboard
trigger1=1
text="%f,%f,%f,%f,%f"
params=screenpos x,screenpos y,abs(screenpos x*0.185),abs(screenpos y*0.185),100-(abs((screenpos x-160)*0.185)+abs((screenpos y-120)*0.185))
ignorehitpause=1
[State *]
type=null;helper
triggerall=numexplod(100020003000)||enemy,numexplod(100020003000)
triggerall=numenemy
triggerall=!numhelper(63000)
triggerall=command="start"
trigger1=roundstate=2&&enemy,var(52)=0&&life>0
helpertype=normal
name="63000"
id=63000
pos=0,0
postype=p1
facing=1
stateno=63000
ownpal=1
pausemovetime=99999999999999999999999
supermovetime=99999999999999999999999
ignorehitpause=1
