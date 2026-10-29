// Included inside shared_metadata.cpp's private namespace after validation helpers.
// Exact gifts prepare block from diagnostics/quick-close.js; fingerprints are
// generated in shared_gift_code.h, included by the parent at global scope.
int do_gifts(Ctx &c) {
  const void **H=c.out->opt_handles;int rc=0;
  auto from_type=reinterpret_cast<void *(*)(const void *)>(c.core->class_from_type);
  auto field_type=reinterpret_cast<void *(*)(const void *)>(c.core->field_get_type);
  auto method_return=reinterpret_cast<void *(*)(const void *)>(c.core->method_get_return_type);
  auto method_param=reinterpret_cast<void *(*)(const void *,uint32_t)>(c.core->method_get_param);
  auto type_class=[&](const void *type)->const void * {return type?from_type(type):nullptr;};
  auto field_class=[&](const void *field)->const void * {return field?type_class(field_type(field)):nullptr;};
  auto return_class=[&](const void *method)->const void * {return method?type_class(method_return(method)):nullptr;};
  auto gift_class=[&](const char *id)->const void * {
    const char *name=nullptr;
    for(const auto &t:pgr_shared_gift::kTypes)if(!std::strcmp(t.id,id)){name=t.canonical;break;}
    if(!name)return nullptr;
    for(size_t i=0;i<kOptClassCount;++i)if(!std::strcmp(kOptClasses[i].canonical,name))return c.out->opt_classes[i];
    return nullptr;
  };
  auto pinned=[&](const void *owner,const char *key,const char *name,const char *ret,TypePred pred,
                  std::initializer_list<MParam> params,int method_slot,int code_slot)->int {
    const pgr_shared_gift::Code *code=nullptr;
    for(const auto &entry:pgr_shared_gift::kCodes)if(!std::strcmp(entry.key,key)){code=&entry;break;}
    if(!code)return set_err(c,PGR_SHARED_ERR_INTERNAL,"gift-code-missing",key);
    return check_method_ex(c,owner,name,ret,pred,uint32_t(params.size()),params.begin(),false,false,
                           code->rva,code->bytes,code->length,&H[method_slot],&H[code_slot]);
  };
  auto gm=[&](const char *id,const char *name,const char *ret,TypePred pred,
              std::initializer_list<MParam> params,int method_slot,int code_slot)->int {
    const std::string key=std::string(id)+"."+name;
    return pinned(gift_class(id),key.c_str(),name,ret,pred,params,method_slot,code_slot);
  };
  auto gf=[&](const char *id,const char *name,uint64_t offset,int slot)->int {
    return ref_field(c,gift_class(id),name,offset,&H[slot]);
  };
  auto typed=[&](const void *owner,const char *name,uint64_t offset,const char *type,int slot)->int {
    return typed_field(c,owner,name,offset,type,"gift-field-type",&H[slot]);
  };
  auto plain=[&](const void *owner,const char *name,const char *ret,TypePred pred,
                 std::initializer_list<MParam> params,bool inflated,int slot)->int {
    return check_method_ex(c,owner,name,ret,pred,uint32_t(params.size()),params.begin(),false,inflated,
                           0,nullptr,0,&H[slot]);
  };
  const char *BOOL="System.Boolean",*INT="System.Int32",*VOID="System.Void";
  const char *STICKER="Holoholo.Rpc.StickerProto",*GIFT="Holoholo.Rpc.GiftBoxProto",*DETAILS="Holoholo.Rpc.GiftBoxDetailsProto";
  const auto data_pred=+[](const std::string &n){return ends_with(n,".Data");};
  const auto initializer_pred=+[](const std::string &n){return ends_with(n,".Initializer");};
  const auto inventory_pred=+[](const std::string &n){return match_generic_1(n,"System.Collections.Generic.List","Holoholo.Rpc.StickerProto");};
  const auto promise_pred=+[](const std::string &n){return match_generic_1(n,"Niantic.Promises.IPromise","Holoholo.Rpc.SendGiftOutProto");};
  const auto sendable_pred=+[](const std::string &n){return match_generic_1(n,"System.Collections.Generic.IEnumerable","Holoholo.Rpc.GiftBoxProto");};
  const auto enumerator_pred=+[](const std::string &n){return match_generic_1(n,"System.Collections.Generic.IEnumerator","Holoholo.Rpc.GiftBoxProto");};
  if((rc=gf("5936","thisData",0x460,H_GF_F_DATA)))return rc;
  const void *data_class=field_class(H[H_GF_F_DATA]);
  if((rc=ref_field(c,data_class,"FriendProto",0x18,&H[H_GF_F_PANEL_FRIEND])))return rc;
#define GIFT_METHOD(ID,NAME,RET,PRED,PARAMS,SLOT) \
  if((rc=gm(ID,NAME,RET,PRED,PARAMS,H_GF_M_##SLOT,H_GF_M_##SLOT##_CODE)))return rc
  GIFT_METHOD("5916","HandleOpenComplete",VOID,nullptr,{},PROF_OPEN);
  GIFT_METHOD("5916","HandleDismiss",VOID,nullptr,{},PROF_CLOSE);
  GIFT_METHOD("5916","get_CurrentlyOpenFriendProfileInfoPanel","Niantic.Holoholo.Social.UI.FriendProfileInfoPanel",nullptr,{},PANEL);
  GIFT_METHOD("5916","get_ActionButtonsDisabled",BOOL,nullptr,{},DISABLED);
  GIFT_METHOD("5936","CheckOnGift",BOOL,nullptr,{},CHECK);
  GIFT_METHOD("5936","OnGiftClick",VOID,nullptr,{},CHOOSE);
  GIFT_METHOD("6240","HandleOpenComplete",VOID,nullptr,{},OPEN_READY);
  GIFT_METHOD("6240","HandleDismiss",VOID,nullptr,{},OPEN_CLOSE);
  GIFT_METHOD("6240","OnOpenClick",VOID,nullptr,{},OPEN);
  GIFT_METHOD("6258","HandleOpenComplete",VOID,nullptr,{},SEND_READY);
  GIFT_METHOD("6258","HandleDismiss",VOID,nullptr,{},SEND_CLOSE);
  GIFT_METHOD("6258","OnSendClick",VOID,nullptr,{},SEND);
  GIFT_METHOD("6258","OnStickersClick",VOID,nullptr,{},STICKERS);
  GIFT_METHOD("6268","Initialize",VOID,nullptr,(std::initializer_list<MParam>{{nullptr,data_pred}}),LIST_INIT);
  GIFT_METHOD("6268","HandleOpenComplete",VOID,nullptr,{},LIST_READY);
  GIFT_METHOD("6268","HandleDismiss",VOID,nullptr,{},LIST_CLOSE);
  GIFT_METHOD("6211","UpdateData",VOID,nullptr,(std::initializer_list<MParam>{{INT,nullptr},{DETAILS,nullptr}}),GIFT_UPDATE);
  GIFT_METHOD("6211","GiftBox",DETAILS,nullptr,(std::initializer_list<MParam>{{INT,nullptr}}),GIFT_BOX);
  GIFT_METHOD("6211","OnClick",VOID,nullptr,(std::initializer_list<MParam>{{INT,nullptr}}),GIFT_CLICK);
  GIFT_METHOD("6176","Reinitialize",VOID,nullptr,(std::initializer_list<MParam>{{nullptr,initializer_pred}}),STICKER_INIT);
  GIFT_METHOD("6176","Click",VOID,nullptr,{},STICKER_CLICK);
  GIFT_METHOD("6195","HandleOpenComplete",VOID,nullptr,{},SHEET_READY);
  GIFT_METHOD("6195","HandleDismiss",VOID,nullptr,{},SHEET_CLOSE);
  GIFT_METHOD("6164","GetStickerInventory",nullptr,inventory_pred,(std::initializer_list<MParam>{{BOOL,nullptr}}),INVENTORY);
  GIFT_METHOD("6258","bmzx",VOID,nullptr,(std::initializer_list<MParam>{{STICKER,nullptr}}),SEL_STICKER);
  GIFT_METHOD("6164","StickerCountExcludingReserved",INT,nullptr,(std::initializer_list<MParam>{{STICKER,nullptr}}),QUANTITY);
  GIFT_METHOD("6164","AnyStickers",BOOL,nullptr,{},ANY_STICKERS);
  if((rc=plain(c.out->opt_classes[OC_BEHAVIOUR],"get_isActiveAndEnabled",BOOL,nullptr,{},false,H_GF_M_ACTIVE)))return rc;
  if((rc=plain(c.out->opt_classes[OC_SELECTABLE],"IsInteractable",BOOL,nullptr,{},false,H_GF_M_INTERACTABLE)))return rc;
  const void *list_data=type_class(method_param(H[H_GF_M_LIST_INIT],0));
  if((rc=ref_field(c,c.out->opt_classes[OC_FRIEND_DETAILS],"player_",0x18,&H[H_GF_F_PLAYER])))return rc;
  if((rc=typed(c.out->opt_classes[OC_PLAYER_SUMMARY],"playerId_",0x18,"System.String",H_GF_F_PLAYER_ID)))return rc;
  if((rc=typed(c.out->opt_classes[OC_GIFT_BOX_DETAILS],"SenderId",0x18,"System.String",H_GF_F_SENDER)))return rc;
  struct RefSpec {const char *id,*name;uint64_t offset;int slot;};
  const RefSpec refs[]={
    {"6240","dlst",0x290,H_GF_F_OPEN_DETAILS},{"6258","dlvg",0x2d0,H_GF_F_SEND_FRIEND},
    {"6258","dlvh",0x2d8,H_GF_F_SEND_STICKER},{"6258","dluq",0x250,H_GF_F_STICKER_SVC},
    {"6240","openButton",0x150,H_GF_F_OPEN_BTN},{"6258","sendButton",0x118,H_GF_F_SEND_BTN},
    {"5936","sendGiftButton",0x160,H_GF_F_CHOOSE_BTN},{"6258","stickerButton",0x150,H_GF_F_STICKER_BTN},
    {"6176","button",0x70,H_GF_F_WIDGET_BTN},{"6176","dlla",0x118,H_GF_F_WIDGET_STICKER}};
  for(const auto &f:refs)if((rc=gf(f.id,f.name,f.offset,f.slot)))return rc;
  if((rc=typed(list_data,"ShowDeleteIcon",0x18,BOOL,H_GF_F_DELETE_MODE)))return rc;
  if((rc=typed(list_data,"IsSurprise",0x28,BOOL,H_GF_F_SURPRISE)))return rc;
  H[H_GF_STICKER_LIST_CLASS]=return_class(H[H_GF_M_INVENTORY]);
  if((rc=plain(H[H_GF_STICKER_LIST_CLASS],"get_Count",INT,nullptr,{},true,H_GF_M_STICKER_COUNT)))return rc;
  if((rc=plain(H[H_GF_STICKER_LIST_CLASS],"get_Item",STICKER,nullptr,{{INT,nullptr}},true,H_GF_M_STICKER_ITEM)))return rc;
  const void *sequence=nullptr;
  if((rc=nested_suffix(c,gift_class("6240"),"ddh",64,true,"gift-presentation-iterator-missing",&sequence)))return rc;
  c.out->opt_classes[OC_GIFT_OPEN_SEQ]=sequence;
  if((rc=ref_field(c,sequence,"<>4__this",0x20,&H[H_GF_F_SEQ_GUI])))return rc;
  if((rc=ref_field(c,sequence,"response",0x28,&H[H_GF_F_SEQ_RESP])))return rc;
  if((rc=typed(sequence,"dlrm",0x10,INT,H_GF_F_SEQ_STATE)))return rc;
  const void *enum_class=nullptr;
  if((rc=enum_i32_field(c,c.out->opt_classes[OC_OPEN_GIFT_OUT],"Result",0x10,&H[H_GF_F_OPEN_RESULT],&enum_class)))return rc;
  if((rc=enum_literal(c,enum_class,"Success",true,"gift-success-literal",&c.out->opt_ints[I_GIFT_OPEN_SUCCESS])))return rc;
  const auto &seq=pgr_shared_gift::kSequence;
  if((rc=check_method(c,sequence,"MoveNext",BOOL,0,nullptr,nullptr,false,false,seq.rva,seq.bytes,seq.length,&H[H_GF_M_SEQ],&H[H_GF_M_SEQ_CODE])))return rc;
  if((rc=ref_field(c,list_data,"GiftBoxDetailsProtos",0x20,&H[H_GF_F_LIST_ITEMS],&H[H_GF_LIST_CLASS])))return rc;
  if((rc=plain(H[H_GF_LIST_CLASS],"get_Count",INT,nullptr,{},true,H_GF_M_LIST_COUNT)))return rc;
  if((rc=plain(H[H_GF_LIST_CLASS],"get_Item",DETAILS,nullptr,{{INT,nullptr}},true,H_GF_M_LIST_ITEM)))return rc;
  if((rc=ref_field(c,gift_class("6268"),"GiftBoxSelected",0x188,&H[H_GF_F_SELECTED],&H[H_GF_SELECTED_CLASS])))return rc;
  if((rc=plain(H[H_GF_SELECTED_CLASS],"Invoke",VOID,nullptr,{{DETAILS,nullptr}},true,H_GF_M_SEL_FIRST)))return rc;
  GIFT_METHOD("6268","ExitGui",VOID,nullptr,{},LIST_EXIT);
  GIFT_METHOD("6258","bnah",VOID,nullptr,(std::initializer_list<MParam>{{"Holoholo.Rpc.SendGiftOutProto",nullptr}}),SEND_RESULT);
  if((rc=enum_i32_field(c,c.out->opt_classes[OC_SEND_GIFT_OUT],"Result",0x10,&H[H_GF_F_SEND_RESULT],&enum_class)))return rc;
  if((rc=enum_literal(c,enum_class,"Success",false,"gift-send-success",&c.out->opt_ints[I_GIFT_SEND_SUCCESS])))return rc;
  if((rc=ref_field(c,gift_class("6268"),"dlwk",0x1a0,&H[H_GF_F_ROWS],&H[H_GF_ROWS_CLASS])))return rc;
  if((rc=typed(gift_class("6268"),"dlwm",0x1b0,BOOL,H_GF_F_LIST_SURPRISE)))return rc;
  if((rc=plain(H[H_GF_ROWS_CLASS],"get_Count",INT,nullptr,{},true,H_GF_M_ROW_COUNT)))return rc;
  if((rc=plain(H[H_GF_ROWS_CLASS],"get_Item","Niantic.Holoholo.Social.Gifting.UI.IGiftItemView2Widget",nullptr,{{INT,nullptr}},true,H_GF_M_ROW_ITEM)))return rc;
  GIFT_METHOD("6149","SendGift",nullptr,promise_pred,(std::initializer_list<MParam>{{"System.UInt64",nullptr},{"System.String",nullptr},{"System.String",nullptr},{STICKER,nullptr},{BOOL,nullptr}}),RPC_SEND);
  GIFT_METHOD("6154","get_SendableGiftsInInventory",nullptr,sendable_pred,{},SENDABLE);
  GIFT_METHOD("6164","ReserveStickers",VOID,nullptr,(std::initializer_list<MParam>{{"System.String",nullptr},{INT,nullptr}}),RESERVE);
  GIFT_METHOD("6164","ReturnReservedStickers",VOID,nullptr,(std::initializer_list<MParam>{{"System.String",nullptr},{INT,nullptr}}),RETURN_STICKER);
  if((rc=gf("5936","dkrv",0x2e0,H_GF_F_PANEL_GIFTING)))return rc;
  if((rc=gf("5936","dkrw",0x2e8,H_GF_F_PANEL_RPC)))return rc;
  if((rc=gf("6149","dlil",0x30,H_GF_F_RPC_STICKERS)))return rc;
  if((rc=typed(c.out->opt_classes[OC_GIFT_BOX_PROTO],"GiftboxId",0x10,"System.UInt64",H_GF_F_GIFT_ID)))return rc;
  if((rc=typed(c.out->opt_classes[OC_STICKER_PROTO],"StickerId",0x10,"System.String",H_GF_F_STICKER_ID)))return rc;
  if((rc=enum_i32_field(c,gift_class("5936"),"dktn",0x450,&H[H_GF_F_CHECK_RESULT],&enum_class)))return rc;
  if((rc=typed(gift_class("5936"),"dkto",0x454,BOOL,H_GF_F_CHECK_PENDING)))return rc;
  if((rc=enum_literal(c,enum_class,"Success",false,"gift-check-success",&c.out->opt_ints[I_GIFT_CHECK_SUCCESS])))return rc;
  const void *iterable=return_class(H[H_GF_M_SENDABLE]);
  if((rc=plain(iterable,"GetEnumerator",nullptr,enumerator_pred,{},true,H_GF_M_GET_ENUM)))return rc;
  const void *enumerator=return_class(H[H_GF_M_GET_ENUM]);
  if((rc=plain(enumerator,"get_Current",GIFT,nullptr,{},true,H_GF_M_CUR_GIFT)))return rc;
  if((rc=plain(c.out->opt_classes[OC_IENUMERATOR],"MoveNext",BOOL,nullptr,{},false,H_GF_M_MOVE_GIFT)))return rc;
  if((rc=plain(c.out->opt_classes[OC_IDISPOSABLE],"Dispose",VOID,nullptr,{},false,H_GF_M_DISP_GIFT)))return rc;
  if((rc=gf("5936","dksq",0x388,H_GF_F_NIA_UTIL)))return rc;
  const void *nia_class=field_class(H[H_GF_F_NIA_UTIL]);
  if(!nia_class)return set_err(c,PGR_MD_ERR_NOT_FOUND,"gift-nia-method");
  auto next_method=reinterpret_cast<void *(*)(const void *,void **)>(c.core->class_get_methods);
  auto method_name=reinterpret_cast<const char *(*)(const void *)>(c.core->method_get_name);
  auto param_count=reinterpret_cast<uint32_t(*)(const void *)>(c.core->method_get_param_count);
  void *iter=nullptr;
  for(unsigned i=0;i<64;++i){
    const void *m=next_method(nia_class,&iter);if(!m)break;
    std::string name;if(!read_utf8(method_name(m),&name))return set_err(c,PGR_MD_ERR_NAME_TOO_LONG,"name-limit");
    if(name!="GetPlayerIdOrNiaAccountId"||param_count(m)!=1)continue;
    std::string parameter,result;
    if(!get_type_name(c,method_param(m,0),&parameter)||!get_type_name(c,method_return(m),&result))return set_err(c,PGR_MD_ERR_NULL_TYPE_NAME,"null-type-name");
    if(parameter=="Niantic.Platform.Internal.Protos.PlayerSummaryProto"&&result=="System.String"){H[H_GF_M_NIA_ID]=m;break;}
  }
  if(!H[H_GF_M_NIA_ID])return set_err(c,PGR_MD_ERR_NOT_FOUND,"gift-nia-method");
  const void *callback=nullptr;
  if((rc=nested_suffix(c,gift_class("6149"),"dcg",64,true,"gift-send-callback",&callback)))return rc;
  H[H_GF_CB_CLASS]=callback;
  if((rc=ref_field(c,callback,"<>4__this",0x10,&H[H_GF_F_CB_SVC])))return rc;
  if((rc=typed(callback,"playerId",0x18,"System.String",H_GF_F_CB_ID)))return rc;
  if((rc=pinned(callback,"6147.bmnz","bmnz",VOID,nullptr,{{"Holoholo.Rpc.SendGiftOutProto",nullptr}},H_GF_M_RPC_RESULT,H_GF_M_RPC_RESULT_CODE)))return rc;
  if((rc=pinned(callback,"6147.bmoa","bmoa",VOID,nullptr,{},H_GF_M_RPC_FINALLY,H_GF_M_RPC_FINALLY_CODE)))return rc;
  const struct Delay{const char *name;uint64_t offset;} delays[]={
    {"dlvj",0x2e4},{"dlvk",0x2e8},{"dlvl",0x2ec},{"dlvm",0x2f0},
    {"stickerButtonFadeDurationSeconds",0x160},{"stickerButtonDelayBeforeFadeSeconds",0x164}};
  for(const auto &delay:delays)if((rc=typed_field(c,gift_class("6258"),delay.name,delay.offset,"System.Single","gift-field-type",nullptr)))return rc;
#undef GIFT_METHOD
  return PGR_MD_OK;
}
