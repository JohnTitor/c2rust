#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <fstream>

// Declares clang::SyntaxOnlyAction.
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
// Declares llvm::cl::extrahelp.
#include "llvm/Support/CommandLine.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <tinycbor/cbor.h>
#include "ast_tags.hpp"

using namespace llvm;
using namespace clang;
using namespace clang::tooling;


using std::string;
using clang::QualType;
using clang::ASTContext;

// Encode a string object assuming that it is valid UTF-8 encoded text
static void cbor_encode_string(CborEncoder *encoder, const std::string &str) {
    auto ptr = str.data();
    auto len = str.size();
    cbor_encode_text_string(encoder, ptr, len);
}

class TranslateASTVisitor;

class TypeEncoder final : public TypeVisitor<TypeEncoder>
{
    ASTContext *Context;
    CborEncoder *encoder;
    TranslateASTVisitor *astEncoder;
    bool isConst;
    
    // Bounds recursion when visiting self-referential record declarations
    std::unordered_set<const clang::RecordDecl*> recordDeclsUnderVisit;
   
    std::unordered_set<const Type*> exports;
    
    bool isUnexported(const Type *ptr) {
        return exports.emplace(ptr).second;
    }
    
    void encodeType(const Type *T, TypeTag tag,
                    std::function<void(CborEncoder*)> extra = [](CborEncoder*){}) {
        if (!isUnexported(T)) return;
        
        CborEncoder local;
        cbor_encoder_create_array(encoder, &local, CborIndefiniteLength);
        
        // 1 - Entity ID
        uintptr_t const_bit = isConst ? 1 : 0;
        cbor_encode_uint(&local, uintptr_t(T) + const_bit);
        
        // 2 - Type tag
        cbor_encode_uint(&local, tag);
        
        // 3 - extras
        extra(&local);
        
        cbor_encoder_close_container(encoder, &local);
    }
    
public:
    explicit TypeEncoder(ASTContext *Context, CborEncoder *encoder, TranslateASTVisitor *ast)
      : Context(Context), encoder(encoder), astEncoder(ast), isConst(false) {}
    
    void VisitQualType(const QualType &QT) {
        if (!QT.isNull()) {
            auto s = QT.split();
            isConst = s.Quals.hasConst();
            Visit(s.Ty);
        }
    }
    
    void VisitEnumType(const EnumType *T) {
        encodeType(T, TagEnumType, [T](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(T->getDecl()));
        });
    }
    
    void VisitConstantArrayType(const ConstantArrayType *T) {
        auto t = T->getElementType();
        auto s = t.split();
        
        encodeType(T, TagConstantArrayType, [T,&s](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(s.Ty));
            cbor_encode_uint(local, T->getSize().getLimitedValue());
        });
        
        VisitQualType(t);
    }
    
    void VisitVariableArrayType(const VariableArrayType *T) {
        auto t = T->getElementType();
        auto s = t.split();

        encodeType(T, TagVariableArrayType, [&s](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(s.Ty));
            cbor_encode_undefined(local); // Variable size not exported currently
        });
        
        VisitQualType(t);
    }
    
    void VisitIncompleteArrayType(const IncompleteArrayType *T) {
        auto t = T->getElementType();
        auto s = t.split();

        encodeType(T, TagIncompleteArrayType, [&s](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(s.Ty));
        });
        
        VisitQualType(t);
    }
    
    // definition below due to recursive call into AST translator
    void VisitRecordType(const RecordType *T);

    
    void VisitBuiltinType(const BuiltinType *T) {
        TypeTag tag;
        using clang::BuiltinType;
        switch (T->getKind()) {
            default:                      tag = TagTypeUnknown; break;
            case BuiltinType::Short:      tag = TagShort;       break;
            case BuiltinType::Int:        tag = TagInt;         break;
            case BuiltinType::Long:       tag = TagLong;        break;
            case BuiltinType::LongLong:   tag = TagLongLong;    break;
            case BuiltinType::UShort:     tag = TagUShort;      break;
            case BuiltinType::UInt:       tag = TagUInt;        break;
            case BuiltinType::ULong:      tag = TagULong;       break;
            case BuiltinType::ULongLong:  tag = TagULongLong;   break;
            case BuiltinType::Double:     tag = TagDouble;      break;
            case BuiltinType::LongDouble: tag = TagLongDouble;  break;
            case BuiltinType::Float:      tag = TagFloat;       break;
            case BuiltinType::SChar:      tag = TagSChar;       break;
            case BuiltinType::UChar:      tag = TagUChar;       break;
            case BuiltinType::Char_U:     tag = TagChar;        break;
            case BuiltinType::Char_S:     tag = TagChar;        break;
            case BuiltinType::Void:       tag = TagVoid;        break;
            case BuiltinType::Bool:       tag = TagBool;        break;
        }
        
        encodeType(T, tag);
    }
    
    // Clang represents function declarations with parameters as `FunctionProtoType`
    // instances whereas functions w/o parameters are handled as `FunctionNoPrototype`
    // instances. Note: we could handle both cases by overriding `VisitFunctionType`
    // instead of the current two-function solution.
    void VisitFunctionProtoType(const FunctionProtoType *T) {
        encodeType(T, TagFunctionType, [T](CborEncoder *local) {
            CborEncoder arrayEncoder;

            // Function types are encoded with an extra list of types. The return type
            // is always the first element of the list followed by the parameters.
            size_t elts = T->getNumParams()+1;
            cbor_encoder_create_array(local, &arrayEncoder, elts);
            
            cbor_encode_uint(&arrayEncoder, uintptr_t(T->getReturnType().getTypePtrOrNull()));
            for (auto x : T->param_types()) {
                auto s = x.split();
                cbor_encode_uint(&arrayEncoder, uintptr_t(s.Ty));
            }
            
            cbor_encoder_close_container(local, &arrayEncoder);
        });
        
        VisitQualType(T->getReturnType());
        for (auto x : T->param_types()) {
            VisitQualType(x);
        }
    }

    // See `VisitFunctionProtoType`.
    void VisitFunctionNoProtoType(const FunctionNoProtoType *T) {
        encodeType(T, TagFunctionType, [T](CborEncoder *local) {
            CborEncoder arrayEncoder;

            cbor_encoder_create_array(local, &arrayEncoder, 1);

            cbor_encode_uint(&arrayEncoder, uintptr_t(T->getReturnType().getTypePtrOrNull()));

            cbor_encoder_close_container(local, &arrayEncoder);
        });

        VisitQualType(T->getReturnType());
    }
    
    void VisitPointerType(const PointerType *T) {
        auto pointee = T->getPointeeType();
        auto s = pointee.split();
        
        encodeType(T, TagPointer, [&s](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(s.Ty));
        });
        
        VisitQualType(pointee);
    }
    
    void VisitTypedefType(const TypedefType *T) {
        auto D = T->getDecl();
        encodeType(T, TagTypedefType, [D](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(D));
        });
    }
    
    void VisitTypeOfType(const TypeOfType *T) {
        auto t = T->desugar();
        auto s = t.split();
        encodeType(T, TagTypeOfType, [&s](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(s.Ty));
        });
        VisitQualType(t);
    }
    
    void VisitElaboratedType(const ElaboratedType *T) {
        auto t = T->desugar();
        auto s = t.split();
        encodeType(T, TagElaboratedType, [&s](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(s.Ty));
        });

        VisitQualType(t);
    }
    
    void VisitDecayedType(const DecayedType *T) {
        auto t = T->desugar();
        auto s = t.split();
        encodeType(T, TagDecayedType, [&s](CborEncoder *local) {
            cbor_encode_uint(local, uintptr_t(s.Ty));
        });
        
        VisitQualType(t);
    }
};

class TranslateASTVisitor final
  : public RecursiveASTVisitor<TranslateASTVisitor> {
      
      ASTContext *Context;
      TypeEncoder typeEncoder;
      CborEncoder *encoder;
      std::unordered_map<string, uint64_t> filenames;
      std::set<std::pair<void*, ASTEntryTag>> exportedTags;
      
      // Returns true when a new entry is added to exportedTags
      bool markForExport(void* ptr, ASTEntryTag tag) {
          return exportedTags.emplace(ptr,tag).second;
      }
      
      void encodeSourcePos(CborEncoder *enc, SourceLocation loc) {
          auto& manager = Context->getSourceManager();
          auto line = manager.getPresumedLineNumber(loc);
          auto col  = manager.getPresumedColumnNumber(loc);
          auto fileid = manager.getFileID(loc);
          auto entry = manager.getFileEntryForID(fileid);
          
          auto filename = string("?");
          if (entry) {
              filename = entry->getName().str();
          }
          
          auto pair = filenames.insert(std::make_pair(filename, filenames.size()));
          
          cbor_encode_uint(enc, pair.first->second);
          cbor_encode_uint(enc, line);
          cbor_encode_uint(enc, col);
      }
      
      // Template required because Decl and Stmt don't share a common base class
      void encode_entry_raw
             (void *ast,
              ASTEntryTag tag,
              SourceLocation loc,
              const Type *ty,
              const std::vector<void *> &childIds,
              std::function<void(CborEncoder*)> extra
             )
      {
          if (!markForExport(ast, tag)) return;
          
          CborEncoder local, childEnc;
          cbor_encoder_create_array(encoder, &local, CborIndefiniteLength);
          
          // 1 - Entry ID
          cbor_encode_uint(&local, uintptr_t(ast));
          
          // 2 - Entry Tag
          cbor_encode_uint(&local, tag);
          
          // 3 - Entry Children
          cbor_encoder_create_array(&local, &childEnc, childIds.size());
          for (auto x : childIds) {
              if (x == nullptr) {
                  cbor_encode_null(&childEnc);
              } else {
                  cbor_encode_uint(&childEnc, uintptr_t(x));
              }
          }
          cbor_encoder_close_container(&local , &childEnc);
          
          // 4 - Line number
          // 5 - Column number
          encodeSourcePos(&local, loc);

          // 6 - Type ID (only for expressions)
          if (nullptr == ty) {
              cbor_encode_null(&local);
          } else {
              cbor_encode_uint(&local, uintptr_t(ty));
          }
          
          // 7 - Extra entries
          extra(&local);
          
          cbor_encoder_close_container(encoder, &local);
      }
      
      void encode_entry
      (Expr *ast,
       ASTEntryTag tag,
       const std::vector<void *> &childIds,
       std::function<void(CborEncoder*)> extra = [](CborEncoder*){}
       ) {
          auto ty = ast->getType();
          encode_entry_raw(ast, tag, ast->getLocStart(), ty.getTypePtrOrNull(), childIds, extra);
          typeEncoder.VisitQualType(ty);
      }
      
      void encode_entry
      (Stmt *ast,
       ASTEntryTag tag,
       const std::vector<void *> &childIds,
       std::function<void(CborEncoder*)> extra = [](CborEncoder*){}
       ) {
          encode_entry_raw(ast, tag, ast->getLocStart(), nullptr, childIds, extra);
      }
      
      void encode_entry
      (Decl *ast,
       ASTEntryTag tag,
       const std::vector<void *> &childIds,
       const Type *T,
       std::function<void(CborEncoder*)> extra = [](CborEncoder*){}
       ) {
          encode_entry_raw(ast, tag, ast->getLocStart(), T, childIds, extra);
      }
      
      
  public:
      explicit TranslateASTVisitor(ASTContext *Context, CborEncoder *encoder)
      : Context(Context), typeEncoder(Context, encoder, this), encoder(encoder) {
      }
      
      const std::unordered_map<string,uint64_t> &getFilenames() const {
          return filenames;
      }
      
      //
      // Statements
      //
      
      bool VisitCompoundStmt(CompoundStmt *CS) {
          std::vector<void*> childIds;
          for (auto x : CS->children()) {
              childIds.push_back(x);
          }

          encode_entry(CS, TagCompoundStmt, childIds);
          return true;
      }

      
      bool VisitReturnStmt(ReturnStmt *RS) {
          std::vector<void*> childIds =
          { RS->getRetValue() } ;
          encode_entry(RS, TagReturnStmt, childIds);
          return true;
      }

      bool VisitDoStmt(DoStmt *S) {
          std::vector<void*> childIds = { S->getBody(), S->getCond() } ;
          encode_entry(S, TagDoStmt, childIds);
          return true;
      }
      
      bool VisitGotoStmt(GotoStmt *GS) {
          std::vector<void*> childIds = { GS->getLabel() };
          encode_entry(GS, TagGotoStmt, childIds);
          return true;
      }
      
      bool VisitLabelStmt(LabelStmt *LS) {
          
          std::vector<void*> childIds = { LS->getSubStmt() };
          encode_entry(LS, TagLabelStmt, childIds,
                             [LS](CborEncoder *array){
                                 cbor_encode_text_stringz(array, LS->getName());
                             });
          return true;
      }

      
      bool VisitNullStmt(NullStmt *NS) {
          std::vector<void*> childIds;
          encode_entry(NS, TagNullStmt, childIds);
          return true;
      }
      
      bool VisitIfStmt(IfStmt *IS) {
          std::vector<void*> childIds = { IS->getCond(), IS->getThen(), IS->getElse() } ;
          encode_entry(IS, TagIfStmt, childIds);
          return true;
      }
      
      bool VisitForStmt(ForStmt *FS) {
          std::vector<void*> childIds =
          { FS->getInit(), FS->getCond(), FS->getInc(), FS->getBody() };
          encode_entry(FS, TagForStmt, childIds);
          return true;
      }
      
      bool VisitWhileStmt(WhileStmt *WS) {
          std::vector<void*> childIds =
          { WS->getCond(), WS->getBody() };
          encode_entry(WS, TagWhileStmt, childIds);
          return true;
      }
      

      bool VisitDeclStmt(DeclStmt *DS) {
          std::vector<void*> childIds(DS->decl_begin(), DS->decl_end());
          encode_entry(DS, TagDeclStmt, childIds);
          return true;
      }

      
      bool VisitBreakStmt(BreakStmt *BS) {
          std::vector<void*> childIds;
          encode_entry(BS, TagBreakStmt, childIds);
          return true;
      }
      
      bool VisitContinueStmt(ContinueStmt *S) {
          std::vector<void*> childIds;
          encode_entry(S, TagContinueStmt, childIds);
          return true;
      }
      
      bool VisitCaseStmt(CaseStmt *CS) {
          std::vector<void*> childIds =
          { CS->getLHS(), CS->getSubStmt() };
          encode_entry(CS, TagCaseStmt, childIds);
          return true;
      }
      
      bool VisitSwitchStmt(SwitchStmt *SS) {
          std::vector<void*> childIds =
          { SS->getCond(), SS->getBody() };
          encode_entry(SS, TagSwitchStmt, childIds);
          return true;
      }
      
      bool VisitDefaultStmt(DefaultStmt *DS) {
          std::vector<void*> childIds = { DS->getSubStmt() };
          encode_entry(DS, TagDefaultStmt, childIds);
          return true;
      }
      
      //
      // Expressions
      //
      
      bool VisitParenExpr(ParenExpr *E) {
          std::vector<void*> childIds { E->getSubExpr() };
          encode_entry(E, TagParenExpr, childIds);
          return true;
      }
      
      bool VisitMemberExpr(MemberExpr *E) {
          std::vector<void*> childIds =
            { E->getBase(), E->getMemberDecl() };
          encode_entry(E, TagMemberExpr, childIds);
          return true;
      }
      
      bool VisitInitListExpr(InitListExpr *ILE) {
          std::vector<void*> childIds;
          for (auto x : ILE->inits()) {
              childIds.push_back(x);
          }
          encode_entry(ILE, TagInitListExpr, childIds);
          
          return true;
      }
      
      bool VisitImplicitValueInitExpr(ImplicitValueInitExpr *E) {
          std::vector<void*> childIds;
          encode_entry(E, TagImplicitValueInitExpr, childIds);
          return true;
      }
      
      bool VisitImplicitCastExpr(ImplicitCastExpr *ICE) {
          std::vector<void*> childIds = { ICE->getSubExpr() };
          encode_entry(ICE, TagImplicitCastExpr, childIds,
                             [ICE](CborEncoder *array){
                                 cbor_encode_text_stringz(array, ICE->getCastKindName());
                             });
          return true;
      }
      
      bool VisitCStyleCastExpr(CStyleCastExpr *E) {
          std::vector<void*> childIds = { E->getSubExpr() };
          encode_entry(E, TagCStyleCastExpr, childIds,
                       [E](CborEncoder *array){
                           cbor_encode_text_stringz(array, E->getCastKindName());
                       });
          return true;
      }
      
      bool VisitUnaryOperator(UnaryOperator *UO) {
          std::vector<void*> childIds = { UO->getSubExpr() };
          encode_entry(UO, TagUnaryOperator, childIds,
                             [UO](CborEncoder *array) {
                                 cbor_encode_string(array, UO->getOpcodeStr(UO->getOpcode()).str());
                                 cbor_encode_boolean(array, UO->isPrefix());
                             });
          return true;
      }
      
      bool VisitBinaryOperator(BinaryOperator *BO) {
          std::vector<void*> childIds = { BO->getLHS(), BO->getRHS() };
          encode_entry(BO, TagBinaryOperator, childIds,
                             [BO](CborEncoder *array) {
                                 cbor_encode_string(array, BO->getOpcodeStr().str());
                             });
          return true;
      }
      
      bool VisitConditionalOperator(ConditionalOperator *CO) {
          std::vector<void*> childIds = { CO->getCond(), CO->getTrueExpr(), CO->getFalseExpr() };
          encode_entry(CO, TagConditionalOperator, childIds);
          return true;
      }
      
      bool VisitBinaryConditionalOperator(BinaryConditionalOperator *CO) {
          std::vector<void*> childIds = { CO->getCommon(), CO->getFalseExpr() };
          encode_entry(CO, TagBinaryConditionalOperator, childIds);
          return true;
      }
      
      bool VisitDeclRefExpr(DeclRefExpr *DRE) {
          std::vector<void*> childIds = { DRE->getDecl() };
          encode_entry(DRE, TagDeclRefExpr, childIds);
          return true;
      }
      
      bool VisitCallExpr(CallExpr *CE) {
          std::vector<void*> childIds = { CE->getCallee() };
          for (auto x : CE->arguments()) {
              childIds.push_back(x);
          }
          encode_entry(CE, TagCallExpr, childIds);
          return true;
      }
      
      bool VisitArraySubscriptExpr(ArraySubscriptExpr *E) {
          std::vector<void*> childIds = { E->getLHS(), E->getRHS() };
          encode_entry(E, TagArraySubscriptExpr, childIds);
          return true;
      }
 
      
      //
      // Declarations
      //
      
      bool VisitFunctionDecl(FunctionDecl *FD)
      {
          std::vector<void*> childIds;
          for (auto x : FD->parameters()) {
              childIds.push_back(x);
          }
          childIds.push_back(FD->getBody());

          auto functionType = FD->getType();
          encode_entry(FD, TagFunctionDecl, childIds, functionType.getTypePtr(),
                             [FD](CborEncoder *array) {
                                 auto name = FD->getNameAsString();
                                 cbor_encode_string(array, name);
                             });
          typeEncoder.VisitQualType(functionType);

          return true;
      }
      
      /* I don't think this adds anything that we don't get from VarDecl
      bool VisitParmVarDecl(ParmVarDecl *PVD)
      {
          std::vector<void*> childIds = { PVD->getDefinition() };
          encode_entry_extra(encoder, PVD, TagParmVarDecl, childIds,
                             [PVD](CborEncoder *array){
                                 auto name = PVD->getNameAsString();
                                 cbor_encode_text_stringz(array, name.c_str());
                             });
          return true;
      }*/
      
      
      bool VisitVarDecl(VarDecl *VD)
      {
          std::vector<void*> childIds =
          { VD->getInit() } ;
          auto T = VD->getType();
          
          encode_entry(VD, TagVarDecl, childIds, T.getTypePtr(),
                             [VD](CborEncoder *array){
                                 auto name = VD->getNameAsString();
                                 cbor_encode_string(array, name);
                             });
          
          typeEncoder.VisitQualType(T);

          return true;
      }
      
      
      bool VisitRecordDecl(RecordDecl *D) {
          std::vector<void*> childIds;
          for (auto x : D->fields()) {
              childIds.push_back(x);
          }
          encode_entry(D, TagRecordDecl, childIds, nullptr,
          [D](CborEncoder *local){
              auto name = D->getNameAsString();
              cbor_encode_string(local, name);
          });
          return true;
      }
      
      bool VisitEnumDecl(EnumDecl *D) {
          std::vector<void*> childIds;
          for (auto x : D->enumerators()) {
              childIds.push_back(x);
          }
          
          encode_entry(D, TagEnumDecl, childIds, nullptr,
          [D](CborEncoder *local){
              auto name = D->getNameAsString();
              cbor_encode_string(local, name);
          });
          return true;
      }
      
      bool VisitEnumConstantDecl(EnumConstantDecl *D) {
          std::vector<void*> childIds = { D->getInitExpr() };
          
          encode_entry(D, TagEnumConstantDecl, childIds, nullptr,
            [D](CborEncoder *local){
              auto name = D->getNameAsString();
              cbor_encode_string(local, name);
          });
          return true;
      }
      
      bool VisitFieldDecl(FieldDecl *D) {
          std::vector<void*> childIds;
          encode_entry(D, TagFieldDecl, childIds, nullptr,
                             [D](CborEncoder *array) {
                                 auto name = D->getNameAsString();
                                 cbor_encode_string(array, name);
                             });
          return true;
      }
      
      bool VisitTypedefDecl(TypedefDecl *D) {
          std::vector<void*> childIds;
          auto typeForDecl = D->getUnderlyingType();
          encode_entry(D, TagTypedefDecl, childIds, typeForDecl.getTypePtrOrNull(),
                             [D](CborEncoder *array) {
                                 auto name = D->getNameAsString();
                                 cbor_encode_string(array, name);
                             });

          typeEncoder.VisitQualType(typeForDecl);
          
          return true;
      }
      
      //
      // Literals
      //
      
      bool VisitIntegerLiteral(IntegerLiteral *IL) {
          std::vector<void*> childIds;
          encode_entry(IL, TagIntegerLiteral, childIds,
                             [IL](CborEncoder *array){
                                 cbor_encode_uint(array, IL->getValue().getLimitedValue());
                             });
          return true;
      }
      
      bool VisitCharacterLiteral(CharacterLiteral *L) {
          std::vector<void*> childIds;
          encode_entry(L, TagCharacterLiteral, childIds,
                             [L](CborEncoder *array){
                                 auto lit = L->getValue();
                                 cbor_encode_uint(array, lit);
                             });
          return true;
      }
      
      bool VisitStringLiteral(clang::StringLiteral *SL) {
          std::vector<void*> childIds;
          encode_entry(SL, TagStringLiteral, childIds,
                             [SL](CborEncoder *array){
                                // C and C++ supports different string types, so 
                                // we need to identify the string literal type
                                switch(SL->getKind()) {
                                    case clang::StringLiteral::StringKind::Ascii:
                                        cbor_encode_uint(array, StringTypeTag::TagAscii);
                                        break;
                                    case clang::StringLiteral::StringKind::Wide:
                                        cbor_encode_uint(array, StringTypeTag::TagWide);
                                        break;
                                    case clang::StringLiteral::StringKind::UTF8:
                                        cbor_encode_uint(array, StringTypeTag::TagUTF8);
                                        break;
                                    case clang::StringLiteral::StringKind::UTF16:
                                        cbor_encode_uint(array, StringTypeTag::TagUTF16);
                                        break;
                                    case clang::StringLiteral::StringKind::UTF32:
                                        cbor_encode_uint(array, StringTypeTag::TagUTF32);
                                        break;
                                }
                                // The size of the wchar_t type in C is implementation defined
                                cbor_encode_uint(array, SL->getCharByteWidth());

                                // String literals can contain arbitrary bytes, so  
                                // we encode these as byte strings rather than text.

                                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(SL->getBytes().data());
                                cbor_encode_byte_string(array, bytes, SL->getByteLength());
                             });
          return true;
      }
      
      bool VisitFloatingLiteral(clang::FloatingLiteral *L) {
          std::vector<void*> childIds;
          encode_entry(L, TagFloatingLiteral, childIds,
                       [L](CborEncoder *array){
                           auto lit = L->getValueAsApproximateDouble();
                           cbor_encode_double(array, lit);
                       });
          return true;
      }
  };

void TypeEncoder::VisitRecordType(const RecordType *T) {
    
    encodeType(T, TagRecordType, [T](CborEncoder *local) {
        cbor_encode_uint(local, uintptr_t(T->getDecl()));
    });
    
    // record type might be anonymous and have no top-level declaration
    // structure declarations can reference themselves, so we need
    // a way to guard against unbounded recursion.
    clang::RecordDecl *D = T->getDecl();
    if(recordDeclsUnderVisit.emplace(D).second) {
        astEncoder->TraverseDecl(D);
        recordDeclsUnderVisit.erase(D);
    }
}

class TranslateConsumer : public clang::ASTConsumer {
    const std::string outfile;
public:
    explicit TranslateConsumer(llvm::StringRef InFile) 
        : outfile(InFile.str().append(".cbor")) { }
    
    virtual void HandleTranslationUnit(clang::ASTContext &Context) {
        
        CborEncoder encoder;
        
        auto process = [&encoder, &Context](uint8_t *buffer, size_t len)
        {
            cbor_encoder_init(&encoder, buffer, len, 0);
            
            CborEncoder array;
            
            // Encode all of the reachable AST nodes and types
            cbor_encoder_create_array(&encoder, &array, CborIndefiniteLength);
            TranslateASTVisitor visitor(&Context, &array);
            auto translation_unit = Context.getTranslationUnitDecl();
            visitor.TraverseDecl(translation_unit);
            cbor_encoder_close_container(&encoder, &array);
            
            // Track all of the top-level declarations
            cbor_encoder_create_array(&encoder, &array, CborIndefiniteLength);
            for (auto d : translation_unit->decls()) {
                cbor_encode_uint(&array, reinterpret_cast<std::uintptr_t>(d));
            }
            cbor_encoder_close_container(&encoder, &array);
            
            // Encode all of the visited file names
            auto filenames = visitor.getFilenames();
            cbor_encoder_create_array(&encoder, &array, filenames.size());
            for (auto &kv : filenames) {
                auto str = kv.first;
                cbor_encode_string(&array, str);
            }
            cbor_encoder_close_container(&encoder, &array);
        };
        
        process(NULL, 0);
        
        auto needed = cbor_encoder_get_extra_bytes_needed(&encoder);
        std::vector<uint8_t> buf(needed);
        
        process(buf.data(), buf.size());
        
        {   
            std::ofstream out(outfile, out.binary | out.trunc);
            out.write(reinterpret_cast<char*>(buf.data()), buf.size());
        }
    }
};

class TranslateAction : public clang::ASTFrontendAction {
    
public:
  virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
    return std::unique_ptr<clang::ASTConsumer>(new TranslateConsumer(InFile));
  }
};

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory MyToolCategory("my-tool options");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterwards.
static cl::extrahelp MoreHelp("\nMore help text...");

int main(int argc, const char **argv) {
  CommonOptionsParser OptionsParser(argc, argv, MyToolCategory);
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  return Tool.run(newFrontendActionFactory<TranslateAction>().get());
}
