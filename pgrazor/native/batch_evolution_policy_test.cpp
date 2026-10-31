#include "batch_evolution_policy.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace pgr_evolution;

static void check(bool condition,const char* label) {
    if(!condition){std::cerr<<"FAILED: "<<label<<'\n';std::exit(1);}
}

static bool accepts(const std::string& wire) {
    Command command;
    return parse(wire.data(),wire.size(),command);
}

int main() {
    Command command;
    check(parse("1|42|1|#Evolve",14,command),"preview command");
    check(command.token==42&&command.mode==1&&command.tag=="#Evolve"&&!command.remove_tag,"v1 parsed fields and removal default");
    check(normalize_tag(command.tag)=="evolve","normalized tag");
    check(accepts("1|0|0|Evolve"),"idle token zero");
    check(parse("2|42|1|0|Evolve",15,command),"v2 removal off");
    check(command.token==42&&command.mode==1&&!command.remove_tag&&command.tag=="Evolve","v2 removal off fields");
    check(parse("2|42|2|1|#Evolve",16,command),"v2 removal on");
    check(command.token==42&&command.mode==2&&command.remove_tag&&command.tag=="#Evolve","v2 removal on fields");
    check(accepts("2|0|0|0|Evolve"),"v2 idle token zero");
    check(!accepts("2|0|2|1|Evolve"),"v2 active token zero");
    check(!accepts("2|1|1|Evolve")&&!accepts("2|1|1||Evolve"),"v2 missing flag");
    check(!accepts("2|1|1|2|Evolve")&&!accepts("2|1|1|x|Evolve")
        &&!accepts("2|1|1|00|Evolve")&&!accepts("2|1|1|-1|Evolve"),"v2 invalid flag");
    check(!accepts("2|1|1|1|")&&!accepts("2|1|1|1|Bad|Tag"),"v2 empty or delimited tag");
    check(accepts(std::string("2|1|1|1|")+std::string(64,'a')),"v2 tag 64 UTF-8 bytes");
    check(!accepts(std::string("2|1|1|1|")+std::string(65,'a')),"v2 tag over 64 UTF-8 bytes");
    std::string multibyteTag;
    for(int i=0;i<32;i++)multibyteTag+="\xC3\xA9";
    check(accepts(std::string("2|1|1|1|")+multibyteTag),"v2 multibyte tag 64 bytes");
    check(!accepts(std::string("2|1|1|1|")+multibyteTag+"\xC3\xA9"),"v2 multibyte tag over 64 bytes");
    check(!accepts("2|1|1|1|Bad\nTag"),"v2 control character");
    check(accepts("1|9223372036854775807|2|Evolve"),"maximum signed token");
    check(!accepts("1|9223372036854775808|2|Evolve"),"token overflow");
    check(!accepts("1|0|1|Evolve"),"active token zero");
    check(!accepts("3|1|1|Evolve")&&!accepts("1|1|3|Evolve"),"version and mode");
    check(!accepts("1|-1|1|Evolve")&&!accepts("1|+1|1|Evolve"),"unsigned token only");
    check(!accepts("1|1|1|")&&!accepts("1|1|1|Bad|Tag"),"empty or delimited tag");
    check(!accepts(std::string("1|1|1|Bad\nTag")),"control character");
    check(!accepts(std::string("1|1|1|")+std::string(65,'a')),"tag byte limit");
    check(!accepts(std::string("1|1|1|")+std::string("\xC0\xAF",2)),"invalid UTF-8");
    check(accepts(std::string("1|1|1|\xC3\xA9")),"valid UTF-8");
    check(!parse(nullptr,0,command),"null buffer");

    check(reconciled_identity(10,10,2,true,2,true,2),"same ID updated");
    check(!reconciled_identity(10,10,2,true,1,true,1),"same ID not updated");
    check(reconciled_identity(10,11,2,false,0,true,2),"replacement with old ID gone");
    check(!reconciled_identity(10,11,2,true,1,true,2),"replacement while old ID remains");
    check(!reconciled_identity(10,11,2,false,0,true,3),"replacement wrong species");
    check(!reconciled_identity(10,11,2,false,0,false,0),"replacement missing from inventory");
    check(!reconciled_identity(10,0,2,true,2,false,0),"zero result ID");
    check(!reconciled_identity(0,11,2,false,0,true,2),"zero original ID");
    check(!reconciled_identity(10,10,0,true,2,true,2),"zero target");

    Budget budget;
    Candidate first;first.id=1;first.species=1;first.target=2;first.family=10;first.candy=12;
    first.item=5;first.item_count=1;
    Candidate second=first;second.id=2;second.species=2;second.target=3;second.candy=8;
    check(budget.reserve(first,20,2),"first reservation");
    check(budget.reserve(second,20,2),"shared family and item stocks");
    check(budget.candy.at(10)==20&&budget.items.at(5)==2,"combined usage");

    Candidate third=first;third.id=3;third.candy=1;
    const auto candy_before=budget.candy,items_before=budget.items;
    check(!budget.reserve(third,20,3),"candy exhausted");
    check(budget.candy==candy_before&&budget.items==items_before,"failed candy reserve is atomic");
    third.candy=0;
    check(!budget.reserve(third,20,2),"item exhausted");
    check(budget.candy==candy_before&&budget.items==items_before,"failed item reserve is atomic");

    Candidate other=first;other.id=4;other.family=11;other.candy=10;
    check(budget.reserve(other,10,3),"separate family shares item stock");
    check(budget.items.at(5)==3,"shared item use across families");
    other.id=0;
    check(!budget.reserve(other,10,4),"invalid candidate ID");
    other.id=5;other.target=0;
    check(!budget.reserve(other,10,4),"invalid target");
    check(budget.items.at(5)==3,"invalid candidate leaves budget unchanged");
    return 0;
}
