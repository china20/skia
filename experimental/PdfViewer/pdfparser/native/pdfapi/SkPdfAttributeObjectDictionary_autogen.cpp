#include "SkPdfAttributeObjectDictionary_autogen.h"


#include "SkPdfNativeDoc.h"
SkString SkPdfAttributeObjectDictionary::O(SkPdfNativeDoc* doc) {
  SkPdfNativeObject* ret = get("O", "");
  if (doc) {ret = doc->resolveReference(ret);}
  if ((ret != NULL && ret->isName()) || (doc == NULL && ret != NULL && ret->isReference())) return ret->nameValue2();
  // TODO(edisonn): warn about missing required field, assert for known good pdfs
  return SkString();
}

bool SkPdfAttributeObjectDictionary::has_O() const {
  return get("O", "") != NULL;
}
